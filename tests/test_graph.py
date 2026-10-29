# Graph tests verify dependency semantics, actual kernel execution, and replay ownership.
# 图测试验证依赖语义、实际 kernel 执行及重放资源归属。
import struct
import unittest
from host_support import HostCase, increment_kernel
from simt_lab import Program
from simt_lab.transport import DeviceError


class GraphTest(HostCase):
    # Build a reusable upload, increment, download chain using real device operations.
    # 使用实际设备操作构造可复用的上传、递增、下载链。
    def chain(self, device, size=32):
        buffer = device.allocate(4*size)
        kernel = device.load(increment_kernel())
        graph = device.graph()
        upload = graph.add(device.op_h2d(buffer,struct.pack('<'+'I'*size,*range(size))))
        compute = graph.add(kernel.operation((buffer,size,7)),(upload,))
        download = graph.add(device.op_d2h(buffer,4*size),(compute,))
        return graph,buffer,kernel,upload,compute,download

    # Instantiate once, then launch repeatedly with one public packet per replay.
    # 实例化一次后重复启动，每次重放只有一个公开提交包。
    def test_instantiate_snapshot_and_replay(self):
        d = self.device()
        graph,buffer,kernel,upload,compute,download = self.chain(d)
        executable = graph.instantiate()
        self.assertEqual(d.stats()['write'],0)
        graph.add(d.op_memset(buffer,0,128),(download,))
        for replay in range(3):
            run = executable.launch()
            self.assertEqual(d.stats()['write'],replay+1)
            self.assertEqual(struct.unpack('<32I',run.node_result(download)),tuple(i+7 for i in range(32)))
            self.assertEqual(len(run.query()['children']),3)
        self.assertEqual(d.stats()['completed'],3)

    # Updating input or kernel parameters preserves node identity and topology.
    # 更新输入或 kernel 参数时保留节点标识和拓扑。
    def test_updates_and_inflight_exclusion(self):
        d = self.device()
        graph,buffer,kernel,upload,compute,download = self.chain(d)
        executable = graph.instantiate()
        run = executable.launch()
        with self.assertRaisesRegex(DeviceError,'graph_in_use'): executable.update(compute,kernel.operation((buffer,32,11)))
        with self.assertRaisesRegex(DeviceError,'buffer_in_use'): buffer.free()
        run.wait()
        executable.update(upload,d.op_h2d(buffer,struct.pack('<32I',*([5]*32))))
        executable.update(compute,kernel.operation((buffer,32,11)))
        self.assertEqual(struct.unpack('<32I',executable.launch().node_result(download)),(16,)*32)
        with self.assertRaisesRegex(DeviceError,'type_change'): executable.update(upload,d.op_empty())

    # Fork/join edges are checked against completion traces, not just final arithmetic.
    # fork/join 依赖通过完成跟踪验证，不只检查最终算术结果。
    def test_graph_branch_overlap_and_join(self):
        d = self.device()
        a,b = d.allocate(4),d.allocate(4096)
        kernel = d.load(increment_kernel(delay=80))
        graph = d.graph()
        root = graph.add(d.op_memset(a,0,4))
        compute = graph.add(kernel.operation((a,1,19),threads=1),(root,))
        copy = graph.add(d.op_h2d(b,bytes([91])*4096),(root,))
        join = graph.add(d.op_d2h(a,4),(compute,copy))
        run = graph.instantiate().launch()
        d.progress(2)
        self.assertNotEqual(d.stats()['compute'],0)
        self.assertNotEqual(d.stats()['copy'],0)
        self.assertEqual(struct.unpack('<I',run.node_result(join)),(19,))
        entries = [entry for entry in d.trace() if entry['parent']==run.id]
        stamps = {(e['node'],e['action']):e['tick'] for e in entries}
        self.assertGreaterEqual(stamps[join,'start'],max(stamps[compute,'complete'],stamps[copy,'complete']))

    # Cycle and dangling-edge rejection does not leave a partially instantiated graph.
    # 拒绝环路和悬空边，不留下部分实例化的图。
    def test_invalid_topology_and_empty_graph(self):
        d = self.device()
        graph = d.graph()
        a = graph.add(d.op_empty())
        b = graph.add(d.op_empty(),(a,))
        graph.depends_on(a,b)
        with self.assertRaisesRegex(DeviceError,'graph_cycle'): graph.instantiate()
        other = d.graph()
        other.add(d.op_empty(),(999,))
        with self.assertRaisesRegex(DeviceError,'invalid_graph_dependency'): other.instantiate()
        self.assertEqual(d.stats()['write'],0)
        self.assertEqual(d.graph().instantiate().launch().wait()['state'],'complete')

    # Node indices need not be topological; a download may precede its producer in the template.
    # 节点索引不必是拓扑顺序；下载可在模板中排在生产者之前。
    def test_non_topological_node_order(self):
        d = self.device()
        buffer = d.allocate(4)
        kernel = d.load(increment_kernel())
        graph = d.graph()
        download = graph.add(d.op_d2h(buffer,4))
        upload = graph.add(d.op_h2d(buffer,struct.pack('<I',23)))
        compute = graph.add(kernel.operation((buffer,1,4),threads=1),(upload,))
        graph.depends_on(download,compute)
        self.assertEqual(struct.unpack('<I',graph.instantiate().launch().node_result(download)),(27,))

    # Replays of one executable serialize across streams without requiring host waits.
    # 同一个可执行图跨 stream 重放时串行化，无需主机逐次等待。
    def test_same_executable_serializes_replays(self):
        d = self.device()
        buffer = d.allocate(4)
        kernel = d.load(increment_kernel())
        graph = d.graph()
        graph.add(kernel.operation((buffer,1,1),threads=1))
        executable = graph.instantiate()
        first,second = executable.launch(stream=1),executable.launch(stream=2)
        second.wait()
        self.assertEqual(struct.unpack('<I',d.d2h(buffer,4,stream=3).result()),(2,))
        events = {(e['job'],e['action']):e['tick'] for e in d.trace()}
        self.assertGreaterEqual(events[second.id,'start'],events[first.id,'complete'])

    # A failed node cancels its descendants while an independent branch still completes.
    # 节点失败取消后继，独立分支仍然完成。
    def test_graph_failure_and_resource_release(self):
        d = self.device()
        buffer = d.allocate(4)
        invalid = d.load(Program().emit('BRA',imm=0))
        graph = d.graph()
        failed = graph.add(invalid.operation(threads=1,max_steps=3))
        cancelled = graph.add(d.op_memset(buffer,7,4),(failed,))
        independent = graph.add(d.op_memset(buffer,9,4))
        run = graph.instantiate().launch()
        with self.assertRaisesRegex(DeviceError,'graph_failed.*step_limit'): run.wait()
        self.assertEqual(run.node_event(cancelled).query()['state'],'cancelled')
        self.assertEqual(run.node_event(independent).query()['state'],'complete')
        buffer.free()

    # Captured stream calls do not enqueue work until the executable graph is launched.
    # 捕获的 stream 调用在可执行图启动之前不入队。
    def test_single_stream_capture_and_replay(self):
        d = self.device()
        buffer = d.allocate(4)
        kernel = d.load(increment_kernel())
        with d.capture(streams=(3,)) as capture:
            d.h2d(buffer,struct.pack('<I',10),stream=3)
            kernel.launch((buffer,1,5),threads=1,stream=3)
            output = d.d2h(buffer,4,stream=3)
        self.assertEqual(d.stats()['write'],0)
        executable = capture.graph.instantiate()
        self.assertEqual(struct.unpack('<I',executable.launch().node_result(output.node)),(15,))

    # Event-based cross-stream fork and rejoin are retained as graph edges.
    # 基于事件的跨 stream 分叉与汇合被保留为图依赖边。
    def test_cross_stream_capture(self):
        d = self.device()
        buffer = d.allocate(4)
        kernel = d.load(increment_kernel())
        with d.capture(streams=(1,2)) as capture:
            d.h2d(buffer,struct.pack('<I',17),stream=1)
            ready = d.record_event(stream=1)
            d.wait_event(ready,stream=2)
            kernel.launch((buffer,1,8),threads=1,stream=2)
            finished = d.record_event(stream=2)
            d.wait_event(finished,stream=1)
            output = d.d2h(buffer,4,stream=1)
        run = capture.graph.instantiate().launch()
        self.assertEqual(struct.unpack('<I',run.node_result(output.node)),(25,))

    # Invalid capture attempts restore normal mode and cannot smuggle in external events.
    # 无效捕获恢复普通模式，且不能混入外部事件。
    def test_capture_misuse_and_recovery(self):
        d = self.device()
        external = d.record_event()
        with self.assertRaisesRegex(DeviceError,'event_not_in_capture'):
            with d.capture(): d.wait_event(external)
        with self.assertRaisesRegex(DeviceError,'unjoined'):
            with d.capture(streams=(1,2)): d.record_event(stream=2)
        with self.assertRaisesRegex(DeviceError,'operation_during_capture'):
            with d.capture(): d.synchronize()
        with self.assertRaisesRegex(DeviceError,'nested_capture'):
            with d.capture():
                with d.capture(): pass
        self.assertEqual(d.stats()['write'],1)
        self.assertEqual(d.record_event().wait()['state'],'complete')

    # Freeing an idle graph's buffer invalidates future launches, including address reuse.
    # 释放空闲图的缓冲区会使未来启动无效，即使地址已被复用。
    def test_freed_buffer_invalidates_replay(self):
        d = self.device()
        graph,buffer,*_ = self.chain(d)
        executable = graph.instantiate()
        buffer.free()
        d.allocate(buffer.size)
        with self.assertRaisesRegex(DeviceError,'invalid_buffer_handle'): executable.launch()


if __name__ == '__main__':
    unittest.main(verbosity=2)

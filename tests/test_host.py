# Validate queue publication, engine progress, memory ownership, and host completion.
# 验证队列发布、引擎推进、内存归属和主机完成通知。
import struct
import unittest
from host_support import HostCase, increment_kernel
from simt_lab import Program
from simt_lab.transport import DeviceError


class HostTest(HostCase):
    # Unpublished queue entries remain invisible; fetching does not complete a long copy.
    # 未发布队列项保持不可见；取走命令不代表长拷贝已经完成。
    def test_doorbell_fetch_and_completion_are_distinct(self):
        d = self.device(auto_doorbell=False)
        buffer = d.allocate(1024)
        upload = d.h2d(buffer,bytes([37])*1024)
        self.assertEqual(d.progress(3)['read'],0)
        self.assertEqual(upload.query()['state'],'queued')
        d.flush()
        stats = d.progress()
        self.assertEqual((stats['read'],stats['completed']),(1,0))
        self.assertEqual(upload.query()['state'],'running')
        self.assertEqual(upload.query()['data'],'')
        upload.wait()
        self.assertEqual(d.d2h(buffer,1024).result(),bytes([37])*1024)

    # Reusing ring slots preserves monotonic publication and completion counters.
    # 复用环形槽位仍保留单调发布与完成计数。
    def test_bounded_ring_and_wraparound(self):
        d = self.device(capacity=2,auto_doorbell=False)
        first,second = d.record_event(),d.record_event()
        with self.assertRaisesRegex(DeviceError,'queue_full'): d.record_event()
        self.assertEqual(d.stats()['write'],2)
        d.flush(); d.progress()
        third = d.record_event()
        third.wait()
        self.assertEqual(first.query()['state'],'complete')
        self.assertEqual(second.query()['state'],'complete')
        self.assertEqual((d.stats()['write'],d.stats()['read'],d.stats()['completed']),(3,3,3))

    # Separate launches consume the same allocation rather than a recreated file image.
    # 分开的启动使用同一个分配，而非重新创建的文件镜像。
    def test_persistent_memory_multiple_kernels_and_two_sms(self):
        d = self.device()
        kernel = d.load(increment_kernel())
        buffer = d.allocate(4*65)
        d.h2d(buffer,struct.pack('<65I',*range(65)))
        first = kernel.launch((buffer,65,3),blocks=3,threads=32)
        second = kernel.launch((buffer,65,7),blocks=3,threads=32)
        result = d.d2h(buffer,4*65).result()
        self.assertEqual(struct.unpack('<65I',result),tuple(i+10 for i in range(65)))
        self.assertGreater(first.query()['instruction_steps'],0)
        self.assertGreater(second.query()['instruction_steps'],0)
        self.assertEqual(d.stats()['sms'],2)

    # Captured host bytes and buffer pins remain valid until completion, not merely fetch.
    # 宿主字节快照与缓冲区保留持续到完成，不止持续到命令被取走。
    def test_buffer_lifetime_reuse_and_host_snapshot(self):
        d = self.device()
        buffer = d.allocate(256)
        data = bytearray([23]*256)
        upload = d.h2d(buffer,data)
        data[:] = bytes(256)
        d.progress()
        with self.assertRaisesRegex(DeviceError,'buffer_in_use'): buffer.free()
        upload.wait()
        self.assertEqual(d.d2h(buffer,256).result(),bytes([23])*256)
        address = buffer.address
        buffer.free()
        replacement = d.allocate(256)
        self.assertEqual(replacement.address,address)
        self.assertEqual(d.d2h(replacement,256).result(),bytes(256))
        with self.assertRaisesRegex(DeviceError,'invalid_buffer_handle'): d.h2d(buffer,b'x')

    # Independent streams overlap a copy with a running kernel without claiming hardware timing.
    # 独立 stream 让拷贝与运行中的 kernel 重叠，不声称模拟硬件时序。
    def test_copy_compute_overlap_and_cross_stream_events(self):
        d = self.device()
        kernel = d.load(increment_kernel(delay=100))
        a,b = d.allocate(4),d.allocate(4096)
        compute = kernel.launch((a,1,11),threads=1,stream=1)
        transfer = d.h2d(b,bytes([19])*4096,stream=2)
        d.progress(2)
        self.assertEqual((d.stats()['compute'],d.stats()['copy']),(compute.id,transfer.id))
        marker = d.record_event(stream=1)
        d.wait_event(marker,stream=2)
        output = d.d2h(a,4,stream=2)
        self.assertEqual(struct.unpack('<I',output.result()),(11,))
        self.assertEqual(transfer.query()['state'],'complete')

    # Failed kernels cancel their dependent stream while unrelated work still finishes.
    # kernel 失败取消依赖 stream，其他独立工作仍会完成。
    def test_failure_propagation_and_independent_progress(self):
        d = self.device()
        failed = d.load(Program().emit('BRA',imm=0)).launch(threads=1,max_steps=5,stream=1)
        dependent = d.record_event(stream=1)
        independent = d.record_event(stream=2)
        with self.assertRaisesRegex(DeviceError,'step_limit'): d.synchronize()
        self.assertEqual(failed.query()['state'],'error')
        self.assertEqual(dependent.query()['state'],'cancelled')
        self.assertEqual(independent.query()['state'],'complete')

    # D2D uses source snapshots and supports overlapping ranges with memmove behavior.
    # D2D 使用源快照，对重叠范围提供 memmove 行为。
    def test_copy_ranges_memset_and_overlap(self):
        d = self.device()
        buffer = d.allocate(8)
        d.h2d(buffer,b'abcdefgh')
        d.d2d(buffer.at(2),buffer,6)
        self.assertEqual(d.d2h(buffer,8).result(),b'ababcdef')
        d.memset(buffer.at(1),255,3)
        self.assertEqual(d.d2h(buffer,8).result(),b'a\xff\xff\xffcdef')
        with self.assertRaisesRegex(DeviceError,'out_of_allocation'): d.d2h(buffer,9)
        with self.assertRaisesRegex(DeviceError,'fill_byte'): d.memset(buffer,256,8)

    # Invalid packets leave pointers and queue positions unchanged and the server reusable.
    # 无效包不改变指针或队列位置，服务器仍可继续使用。
    def test_protocol_validation_and_wait_timeout(self):
        d = self.device()
        for request,error in [('ALLOC 4 extra','unexpected'),('SUBMIT 0 1 999 EMPTY','invalid_job'),
                              ('LOAD zz','invalid_hex'),('ALLOC -1','invalid_protocol_integer')]:
            with self.assertRaisesRegex(DeviceError,error): d._request(request)
        self.assertEqual(d.stats()['write'],0)
        kernel = d.load(increment_kernel(delay=50))
        buffer = d.allocate(4)
        event = kernel.launch((buffer,1,2),threads=1)
        with self.assertRaisesRegex(DeviceError,'wait_timeout'): event.wait(max_ticks=1)
        self.assertNotEqual(event.query()['state'],'complete')
        event.wait()
        self.assertEqual(struct.unpack('<I',d.d2h(buffer,4).result()),(2,))

    # Resource handles cannot accidentally cross independent device processes.
    # 资源句柄不能意外跨越独立设备进程。
    def test_device_ownership(self):
        a,b = self.device(),self.device()
        buffer = a.allocate(4)
        with self.assertRaisesRegex(DeviceError,'foreign_device'): b.h2d(buffer,b'1234')
        with self.assertRaisesRegex(DeviceError,'foreign_device'): b.submit(a.op_empty())
        with self.assertRaisesRegex(DeviceError,'foreign'): b.wait_event(a.record_event())


if __name__ == '__main__':
    unittest.main(verbosity=2)

# Reuse host and graph contracts with actual device clocks, then test persistent hardware lifetimes.
# 使用实际设备时钟复用主机与图契约，再验证持久硬件生命周期。
import random
import struct
import unittest
import test_host as functional_host
import test_graph as functional_graph
from host_support import HostCase, increment_kernel
from simt_lab import Program, Z
from simt_lab.transport import DeviceError
from simt_lab.tensor import MatrixDescriptor
from tensor_support import tensor_program
from simt_lab.workloads import vector_add, reduction, gemm
from simt_lab.transformer import tiny_transformer


class TimedDevice:
    def device(self, **options):
        return super().device(timing=True, hardware_trace=True, **options)


class TimedHostContracts(TimedDevice, functional_host.HostTest):
    pass


class TimedGraphContracts(TimedDevice, functional_graph.GraphTest):
    pass


def read_kernel():
    return Program().emit('LDC', rd=0, ra=Z, mode=4).emit('LDG', rd=1, ra=0, mode=4).emit('EXIT')


# Ordinary encoded instructions connect the host copy, asynchronous DMA, and Tensor array paths.
# 普通编码指令连接主机拷贝、异步 DMA 和 Tensor 阵列通路。
def dma_tensor_kernel():
    p = tensor_program(MatrixDescriptor(0, 1, 1, 2, 128, 136, space=0), columns=8)
    p.emit('LDC', rd=5, ra=Z, mode=4).emit('LDC', rd=6, ra=Z, imm=4, mode=4)
    p.emit('MOVI', rd=1, imm=1024).emit('MOVI', rd=2, imm=128)
    p.emit('MB_INIT', ra=1, imm=1).emit('MB_EXPECT', ra=1, imm=16).emit('MB_ARRIVE', rd=3, ra=1)
    p.emit('CP_ASYNC', ra=5, rb=2, rc=1, imm=16).emit('MB_WAIT', ra=1, rb=3)
    p.emit('MMA', ra=Z).emit('LDT', rd=4, ra=Z, rb=Z)
    return p.emit('STG', ra=6, rb=4, mode=4).emit('EXIT')


class PersistentHardwareTest(TimedDevice, HostCase):
    def test_all_compositional_workloads_through_graph(self):
        d = self.device()
        for factory in (vector_add, reduction, gemm, tiny_transformer):
            buffer = d.allocate(len(factory().image.data))
            workload = factory(base=buffer.address)
            kernel = d.load(workload.program)
            graph = d.graph()
            upload = graph.add(d.op_h2d(buffer, workload.image.data))
            compute = graph.add(kernel.operation(blocks=workload.blocks, threads=workload.threads,
                                                 buffers=(buffer,)), (upload,))
            output = buffer.at(workload.output-buffer.address)
            download = graph.add(d.op_d2h(output, 4*len(workload.expected)), (compute,))
            run = graph.instantiate().launch()
            code = 'f' if workload.kind == 'float' else 'I'
            actual = struct.unpack('<'+code*len(workload.expected), run.node_result(download))
            for value, expected in zip(actual, workload.expected):
                delta = workload.atol+workload.rtol*abs(expected) if code == 'f' else 0
                self.assertAlmostEqual(value, expected, delta=delta, msg=workload.name)
            hardware = run.node_event(compute).query()['hardware']
            if workload.name == 'transformer':
                self.assertEqual(hardware['async'][0]['write_bytes'], 2176)
                self.assertEqual(hardware['tensor'][0]['products'], 2304)
            buffer.free()

    def test_cache_retained_between_readonly_kernels(self):
        d = self.device()
        buffer = d.allocate(4)
        d.h2d(buffer, struct.pack('<I', 29)).wait()
        kernel = d.load(read_kernel())
        cold = kernel.launch((buffer,), threads=1).wait()['hardware']
        warm = kernel.launch((buffer,), threads=1).wait()['hardware']
        self.assertLess(warm['cycles'], cold['cycles'])
        self.assertEqual(warm['dram']['data_reads'], cold['dram']['data_reads'])
        self.assertEqual(warm['l1'][0]['hits'], cold['l1'][0]['hits'] + 1)
        self.assertEqual(warm['clock_start'], cold['clock_end'])

    def test_upload_invalidates_warm_kernel_cache(self):
        d = self.device()
        buffer = d.allocate(4)
        d.load(read_kernel()).launch((buffer,), threads=1).wait()
        d.h2d(buffer, struct.pack('<I', 91))
        d.load(increment_kernel()).launch((buffer, 1, 2), threads=1)
        self.assertEqual(struct.unpack('<I', d.d2h(buffer, 4).result()), (93,))

    def test_reallocation_clears_l1_and_l2(self):
        d = self.device()
        buffer = d.allocate(4)
        d.h2d(buffer, b'\xff'*4)
        d.load(read_kernel()).launch((buffer,), threads=1).wait()
        address = buffer.address
        buffer.free()
        replacement = d.allocate(4)
        self.assertEqual(replacement.address, address)
        self.assertEqual(d.d2h(replacement, 4).result(), bytes(4))
        d.load(increment_kernel()).launch((replacement, 1, 7), threads=1)
        self.assertEqual(struct.unpack('<I', d.d2h(replacement, 4).result()), (7,))

    def test_local_cache_tags_do_not_alias_later_launches(self):
        d = self.device()
        output = d.allocate(4)
        prime = Program().emit('MOVI', rd=0, imm=77).emit('STL', ra=Z, rb=0, mode=4)
        prime.emit('LDL', rd=1, ra=Z, mode=4).emit('EXIT')
        d.load(prime).launch(threads=1).wait()
        read = Program().emit('LDC', rd=0, ra=Z, mode=4).emit('LDL', rd=1, ra=Z, mode=4)
        d.load(read.emit('STG', ra=0, rb=1, mode=4).emit('EXIT')).launch((output,), threads=1)
        self.assertEqual(d.d2h(output, 4).result(), bytes(4))

    def test_one_clock_for_both_engines_and_idle(self):
        d = self.device()
        a, b = d.allocate(4), d.allocate(4096)
        d.load(increment_kernel(delay=100)).launch((a, 1, 1), threads=1, stream=1)
        d.h2d(b, bytes(4096), stream=2)
        self.await_overlap(d)
        for count in (1, 9, 37):
            before = d.stats()
            after = d.progress(count)
            self.assertEqual(after['ticks'] - before['ticks'], count)
            self.assertEqual(after['hardware']['cycles'], after['ticks'])
        d.synchronize()
        before = d.stats()['hardware']['cycles']
        self.assertEqual(d.progress(17)['hardware']['cycles'], before + 17)

    def test_host_link_latency_and_bounded_copy_chunks(self):
        d = self.device()
        buffer = d.allocate(260)
        upload = d.h2d(buffer, bytes([13])*260)
        upload.wait()
        download = d.d2h(buffer, 260)
        self.assertEqual(download.result(), bytes([13])*260)
        transfers = [e for e in d.trace() if e['job'] == upload.id]
        chunks = [e['tick'] for e in transfers if e['action'] == 'copy_chunk']
        writes = [e['tick'] for e in transfers if e['action'] == 'copy_write_submit']
        self.assertEqual([write-chunk for write, chunk in zip(writes, chunks)], [16, 16, 9])
        reads = [e['tick'] for e in d.trace() if e['job'] == download.id and e['action'] == 'copy_read_complete']
        self.assertEqual(download.query()['finished']-reads[-1], 9)
        self.assertEqual(d.stats()['hardware']['dram_writes'], 9)

    def test_random_unaligned_copies_and_overlapping_memmove(self):
        d = self.device()
        buffer = d.allocate(1024)
        rng = random.Random(197)
        reference = bytearray(rng.randbytes(1024))
        d.h2d(buffer, reference)
        cases = [(4, 0, 700), (0, 4, 700), (3, 0, 700), (0, 3, 700), (7, 7, 513), (0, 1024, 0)]
        cases += [(rng.randrange(200), rng.randrange(200), rng.randrange(700)) for _ in range(12)]
        for destination, source, size in cases:
            d.d2d(buffer.at(destination), buffer.at(source), size)
            reference[destination:destination+size] = bytes(reference[source:source+size])
            self.assertEqual(d.d2h(buffer, 1024).result(), bytes(reference))
        d.h2d(buffer.at(1), b'abcde')
        d.memset(buffer.at(7), 0xAB, 515)
        reference[1:6], reference[7:522] = b'abcde', bytes([0xAB])*515
        self.assertEqual(d.d2h(buffer.at(1), 1001).result(), bytes(reference[1:1002]))

    def test_graph_decoder_and_dispatch_have_finite_bandwidth(self):
        d = self.device()
        graph = d.graph()
        for _ in range(9): graph.add(d.op_empty())
        run = graph.instantiate().launch()
        self.assertEqual(run.wait()['state'], 'complete')
        events = [e for e in d.trace() if e['parent'] == run.id]
        decoded = [e['tick'] for e in events if e['action'] == 'graph_node']
        started = [e['tick'] for e in events if e['action'] == 'start']
        self.assertEqual(len(set(decoded)), 9)
        self.assertEqual(len(set(started)), 9)
        self.assertGreaterEqual(max(decoded) - min(decoded), 8)

    def test_decoded_packet_capacity_applies_backpressure(self):
        d = self.device()
        buffer = d.allocate(4096)
        d.h2d(buffer, bytes(4096))
        for _ in range(31): d.record_event()
        stats = d.progress(40)
        self.assertEqual((stats['read'], stats['completed']), (16, 0))
        d.synchronize()
        self.assertEqual(d.stats()['completed'], 32)

    def test_graph_replay_runs_dma_tensor_and_host_copies(self):
        d = self.device()
        source, output = d.allocate(16), d.allocate(4)
        kernel = d.load(dma_tensor_kernel())
        graph = d.graph()
        upload = graph.add(d.op_h2d(source, struct.pack('<4f', 2, 3, 5, 7)))
        compute = graph.add(kernel.operation((source, output), threads=1), (upload,))
        download = graph.add(d.op_d2h(output, 4), (compute,))
        executable = graph.instantiate()
        for _ in range(3):
            run = executable.launch()
            self.assertEqual(struct.unpack('<f', run.node_result(download)), (31,))
            hardware = run.node_event(compute).query()['hardware']
            self.assertEqual(hardware['tensor'][0]['products'], 2)
            self.assertEqual(hardware['async'][0]['write_bytes'], 16)

    # An accepted atomic must finish before a failed kernel releases its buffer or compute engine.
    # 已接受原子请求必须在失败 kernel 释放缓冲区或计算引擎之前完成。
    def test_fault_drains_accepted_atomic_before_releasing_pins(self):
        d = self.device()
        buffer = d.allocate(4)
        p = Program().emit('NOP').emit('NOP').emit('LDC', rd=0, ra=Z, mode=4).emit('MOVI', rd=1, imm=1)
        p.emit('ATOM', rd=2, ra=0, rb=1)
        for _ in range(3): p.emit('NOP')
        failed = d.load(p.emit('EXIT')).launch((buffer,), max_steps=7, stream=1)
        scratch = d.allocate(257)
        transfer = d.h2d(scratch, bytes([19])*257, stream=2)
        for _ in range(10000):
            d.progress()
            if failed.query()['recovering']: break
        else: self.fail('fault never entered drain state')
        self.assertLess(failed.query()['hardware']['global_atomic']['completed'],32)
        with self.assertRaisesRegex(DeviceError, 'buffer_in_use'): buffer.free()
        with self.assertRaisesRegex(DeviceError, 'step_limit'): failed.wait()
        report = failed.query()
        self.assertEqual(report['hardware']['global_atomic']['completed'], 32)
        transfer.wait()
        self.assertEqual(d.d2h(scratch, 257, stream=2).result(), bytes([19])*257)
        self.assertEqual(struct.unpack('<I', d.d2h(buffer, 4, stream=2).result()), (32,))
        d.load(increment_kernel()).launch((buffer, 1, 7), threads=1, stream=2).wait()
        self.assertEqual(struct.unpack('<I', d.d2h(buffer, 4, stream=2).result()), (39,))
        buffer.free()

    def test_fault_retains_local_backing_until_write_acknowledgement(self):
        d = self.device()
        p = Program().emit('MOVI', rd=0, imm=123).emit('STL', ra=Z, rb=0, mode=4)
        for _ in range(50): p.emit('NOP')
        failed = d.load(p.emit('EXIT')).launch(max_steps=20, stream=1)
        with self.assertRaisesRegex(DeviceError, 'step_limit'): failed.wait()
        hardware = failed.query()['hardware']
        self.assertEqual(hardware['dram']['writes'], 4)
        self.assertTrue(any(e['action'] == 'local_submit' for e in hardware['trace']))
        p = Program().emit('LDL', rd=0, ra=Z, mode=4).emit('EXIT')
        self.assertEqual(d.load(p).launch(stream=2).wait()['state'], 'complete')


if __name__ == '__main__':
    unittest.main(verbosity=2)

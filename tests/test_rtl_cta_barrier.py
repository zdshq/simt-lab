# Real instruction and host execution must traverse the finite CTA table and physical frontend wake ports.
# 实际指令与主机执行必须经过有限 CTA 表和物理前端唤醒端口。
import struct
import unittest
import test_memory
import test_timing
from support import SimulationCase
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


class RtlBarrier:
    execution_timeout = 120

    # Correlate table traffic with retired ISA operations, including commands with no participating lanes.
    # 将表流量与实际 ISA 操作对应，包括没有参与 lane 的命令。
    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            units = report['hardware']['cta_barriers']; self.assertTrue(units)
            for unit in units:
                self.assertEqual((unit['backend'], unit['storage_bits'], unit['command_ports']), ('rtl', 18999, 1))
                self.assertEqual(unit['live_threads'], 0)
                self.assertLessEqual(unit['active_clocks'], unit['clocks'])
            expected = sum(report['op_counts'].get(name, 0) for name in ('BAR_SYNC', 'BAR_ARRIVE', 'EXIT'))
            self.assertEqual(sum(unit['commands'] for unit in units), expected)
            self.assertEqual(sum(unit['initializations'] for unit in units), report['blocks_completed'])
        return report


class BarrierContracts(RtlBarrier, SimulationCase):
    test_divergent_barrier_and_generation_reuse = test_memory.MemoryTest.test_divergent_barrier_and_generation_reuse
    test_early_exit_and_explicit_count = test_memory.MemoryTest.test_early_exit_and_explicit_count

    # Concurrent partitions can complete adjacent generations; delay one warp for the duplicate-arrival case.
    # 并发分区可能完成相邻两代屏障；重复到达负例需要显式延迟一个 warp。
    def test_arrival_only_and_misuse(self):
        p = Program().emit('S2R', rd=0, imm=2).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('BRA', imm='producer', guard=0).emit('BAR_SYNC', rd=0, imm=64).emit('EXIT')
        p.label('producer').emit('BAR_ARRIVE', rd=0, imm=64).emit('EXIT')
        self.execute(p, '--threads', 64)
        test_timing.TimedMemoryTest.test_arrival_misuse(self)


class PhysicalBarriers(RtlBarrier, SimulationCase):
    # Four partitions may retire together, but one CTA table can accept only one command per cycle.
    # 四个分区可能同时退休，但一个 CTA 表每拍只能接收一个命令。
    def test_shared_port_backpressure_and_hardware_wake(self):
        p = Program().emit('BAR_SYNC', rd=15).emit('EXIT')
        result = self.execute(p, '--threads', 128, '--partitions', 4)
        hardware = result['hardware']; units = hardware['cta_barriers']
        self.assertGreater(hardware['cta_barrier_stalls'], 0)
        self.assertEqual(sum(unit['completions'] for unit in units), 1)
        for field in ('cta_blocked', 'cta_woken'):
            self.assertEqual(sum(unit[field] for unit in hardware['warp_frontends']), 128)
        self.assertEqual(sum(unit['external_lanes'] for unit in hardware['warp_frontends']), 0)

    # Full barrier-ID capacity, tail warps and repeated CTA allocation share the same physical table slots.
    # 全部屏障编号、尾部 warp 和重复 CTA 分配共用同一组物理表槽。
    def test_sixteen_barriers_tail_warps_and_slot_reuse(self):
        p = Program()
        for barrier in range(16): p.emit('BAR_SYNC', rd=barrier)
        p.emit('EXIT')
        for partitions in (1, 2, 4, 8):
            result = self.execute(p, '--blocks', 9, '--threads', 37, '--max-ctas', 1, '--partitions', partitions)
            units = result['hardware']['cta_barriers']
            self.assertEqual(len(units), 2)
            self.assertEqual(sum(unit['completions'] for unit in units), 9*16)
            self.assertEqual(sum(unit['arrival_lanes'] for unit in units), 9*37*16)
            self.assertEqual(sum(unit['exit_lanes'] for unit in units), 9*37)
            self.assertTrue(all(record['generation'] == 1 for unit in units for record in unit['records']))
            self.assertEqual(sum(unit['cta_woken'] for unit in result['hardware']['warp_frontends']), 9*37*16)

    # One arrived thread can exit while sixteen dynamic records still await the other threads.
    # 一个已到达线程可以退出，此时十六个动态记录仍等待其他线程。
    def test_arrived_exit_retains_membership_for_all_records(self):
        p = Program().emit('S2R', rd=0, imm=1).emit('ISETP', rd=0, ra=0, rb=Z)
        for barrier in range(16): p.emit('BAR_ARRIVE', rd=barrier, guard=0)
        p.emit('EXIT')
        for policy in ('rr', 'min', 'max'):
            result = self.execute(p, '--threads', 64, '--warp-policy', policy)
            self.assertEqual(sum(unit['completions'] for unit in result['hardware']['cta_barriers']), 16)
            self.assertEqual(sum(unit['cta_blocked'] for unit in result['hardware']['warp_frontends']), 0)

    def test_inactive_descriptor_and_overflow(self):
        p = Program().emit('ISETP', rd=0, ra=Z, rb=Z, mode=2)
        p.emit('BAR_SYNC', rd=63, imm=100000, guard=0).emit('EXIT')
        result = self.execute(p)
        self.assertEqual(sum(unit['arrival_lanes'] for unit in result['hardware']['cta_barriers']), 0)
        self.execute(Program().emit('BAR_SYNC', imm=16), error='barrier_arrival_overflow')


# Shared publication across a partial second warp must survive Graph replay and a preceding failed kernel.
# 跨部分第二个 warp 的 shared 发布必须在 Graph 重放和先前 kernel 失败后保持正确。
class BarrierHost(HostCase):
    def output_program(self):
        p = Program().emit('LDC', rd=6, ra=Z, mode=4).emit('S2R', rd=0, imm=1).emit('MOVI', rd=1, imm=4)
        p.emit('IMUL', rd=2, ra=0, rb=1).emit('STS', ra=2, rb=0, mode=4)
        for barrier in range(16): p.emit('BAR_SYNC', rd=barrier)
        p.emit('LDS', rd=3, ra=Z, imm=36*4, mode=4).emit('S2R', rd=1, imm=3).emit('MOVI', rd=4, imm=37)
        p.emit('IMAD', rd=5, ra=1, rb=4, rc=0).emit('IADD', rd=3, ra=3, rb=5).emit('MOVI', rd=4, imm=4)
        return p.emit('IMAD', rd=5, ra=5, rb=4, rc=6).emit('STG', ra=5, rb=3, mode=4).emit('EXIT')

    def test_replayed_shared_publication_after_failed_barrier(self):
        device = self.device(timing=True, register_rtl=True, collector_rtl=True, request_timeout=120)
        bad = device.load(Program().emit('BAR_SYNC', imm=16)).launch()
        with self.assertRaisesRegex(DeviceError, 'barrier_arrival_overflow'): bad.wait()
        output = device.allocate(333*4); module = device.load(self.output_program()); graph = device.graph()
        clear = graph.add(device.op_h2d(output, bytes(output.size)))
        compute = graph.add(module.operation((output,), blocks=9, threads=37), (clear,))
        download = graph.add(device.op_d2h(output, output.size), (compute,)); executable = graph.instantiate()
        for _ in range(2):
            run = executable.launch(stream=2)
            self.assertEqual(struct.unpack('<333I', run.node_result(download)), tuple(range(36,369)))
            hardware = run.node_event(compute).query()['hardware']
            self.assertEqual(sum(unit['completions'] for unit in hardware['cta_barriers']), 9*16)
            self.assertEqual(sum(unit['cta_woken'] for unit in hardware['warp_frontends']), 9*37*16)


if __name__ == '__main__':
    unittest.main(verbosity=2)

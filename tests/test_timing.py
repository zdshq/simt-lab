# Check actual instruction execution through finite hardware resources and independent references.
# 通过有限硬件资源执行真实指令，并与独立参考结果比较。
import random
import struct
import unittest
from support import SimulationCase, word
from test_control import ControlTest as ExistingControlTests
from test_memory import reduction, MemoryTest as ExistingMemoryTests
from test_simulator import SimulationTest as ExistingScalarTests
from simt_lab import Program, Z, NOT_PT


# Select a dynamic instruction's trace event without relying on internal C++ objects.
# 从轨迹选择动态指令事件，不依赖 C++ 内部对象。
def events(report, pc, action):
    return [e['cycle'] for e in report['hardware']['trace'] if e['pc'] == pc and e['action'] == action]


class TimingTest(SimulationCase):
    def timed(self, program, *options, **kwargs):
        return self.execute(program, '--hardware-trace', *options, **kwargs)

    # A dependent chain pays ALU latency while independent operations overlap.
    # 依赖链需等待 ALU 延迟，独立操作则可以重叠。
    def test_dependency_chain(self):
        dependent, independent = Program(), Program()
        for index in range(12):
            dependent.emit('IADD', rd=0, ra=0, rb=Z)
            independent.emit('IADD', rd=index, ra=Z, rb=Z)
        slow = self.timed(dependent.emit('EXIT'), '--threads', 1, '--int-latency', 40)['hardware']
        fast = self.timed(independent.emit('EXIT'), '--threads', 1, '--int-latency', 40)['hardware']
        self.assertGreater(slow['cycles'], fast['cycles'] * 2)
        self.assertGreater(slow['scoreboard_stalls'], 0)
        self.assertEqual(fast['scoreboard_stalls'], 0)
        self.assertEqual(slow['issued'], slow['retired'])

    def test_load_latency_and_waw(self):
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('MOVI', rd=1, imm=9)
        p.emit('IADD', rd=2, ra=0, rb=1).emit('MOVI', rd=0, imm=99).emit('EXIT')
        result = self.timed(p, '--threads', 1, images={'global': struct.pack('<I', 33)})
        self.assertEqual([self.registers(result, r)[0] for r in range(3)], [99, 9, 42])
        self.assertLess(events(result, 1, 'retire')[0], events(result, 0, 'retire')[0])
        self.assertGreaterEqual(events(result, 2, 'issue')[0], events(result, 0, 'retire')[0])
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('MOVI', rd=0, imm=99).emit('EXIT')
        result = self.timed(p, '--threads', 1)
        self.assertEqual(self.registers(result, 0)[0], 99)
        self.assertGreaterEqual(events(result, 1, 'issue')[0], events(result, 0, 'retire')[0])

    # A delayed same-bank source must retain its old value despite a younger write.
    # 同 bank 源操作数读取延迟时，较新写入不能破坏旧值。
    def test_operand_collection_war(self):
        p = Program().emit('MOVI', rd=0, imm=3).emit('MOVI', rd=4, imm=7)
        p.emit('IMAD', rd=8, ra=0, rb=4, rc=Z).emit('MOVI', rd=4, imm=99).emit('EXIT')
        result = self.timed(p, '--threads', 1)
        self.assertEqual(self.registers(result, 8)[0], 21)
        self.assertEqual(self.registers(result, 4)[0], 99)
        reads = events(result, 2, 'operand_read')
        self.assertEqual(len(set(reads)), 2)
        self.assertGreaterEqual(events(result, 3, 'issue')[0], max(reads))

    def test_register_banks_and_duplicate_sources(self):
        reports = []
        for other in (4, 1, 0):
            p = Program().emit('IADD', rd=8, ra=0, rb=other).emit('EXIT')
            reports.append(self.timed(p, '--threads', 1))
        self.assertGreater(reports[0]['hardware']['cycles'], reports[1]['hardware']['cycles'])
        self.assertEqual(len(set(events(reports[1], 0, 'operand_read'))), 1)
        self.assertEqual(len(events(reports[2], 0, 'operand_read')), 1)

    # Different pipe latencies can collide at one physical write bank.
    # 不同流水线延迟可能在同一物理写 bank 汇合并产生冲突。
    def test_writeback_contention(self):
        p = Program().emit('FADD', rd=4, ra=Z, rb=Z).emit('MOVI', rd=2, imm=3)
        p.emit('IADD', rd=8, ra=Z, rb=Z).emit('EXIT')
        result = self.timed(p, '--threads', 1, '--fp-latency', getattr(self, 'writeback_float_latency', 6))
        self.assertGreater(result['hardware']['writeback_stalls'], 0)
        self.assertNotEqual(events(result, 0, 'retire'), events(result, 2, 'retire'))

    def test_sfu_throughput_and_collectors(self):
        p = Program()
        for reg in range(12):
            p.emit('FEXP2', rd=reg, ra=Z)
        p.emit('EXIT')
        result = self.timed(p, '--threads', 1, '--collectors', 1)
        fast = self.timed(p, '--threads', 1, '--sfu-interval', 1)
        starts = [events(result, pc, 'execute')[0] for pc in range(12)]
        self.assertTrue(all(b - a >= 4 for a, b in zip(starts, starts[1:])))
        self.assertGreater(result['hardware']['collector_stalls'], 0)
        self.assertGreater(result['hardware']['cycles'], fast['hardware']['cycles'])
        self.assertEqual([self.registers(result, r)[0] for r in range(12)], [0x3F800000]*12)

    def test_multi_cta_residency(self):
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('EXIT')
        options = ('--blocks', 8, '--threads', 64, '--shared-bytes', 256, '--max-ctas', 2)
        result = self.timed(p, *options)
        self.assertEqual(result['hardware']['peak_ctas_per_sm'], 2)
        self.assertEqual(len(result['ctas']), 8)
        for sm in range(2):
            active = set()
            for event in result['hardware']['trace']:
                if event['sm'] != sm:
                    continue
                if event['action'] == 'admit':
                    self.assertNotIn(event['block'], active)
                    active.add(event['block'])
                    self.assertLessEqual(len(active), 2)
                elif event['action'] == 'cta_retire':
                    active.remove(event['block'])
            self.assertFalse(active)
        limited = self.timed(p, *options, '--sm-registers', 4096)
        self.assertEqual(limited['hardware']['peak_ctas_per_sm'], 1)
        self.timed(p, '--threads', 64, '--sm-registers', 2048, error='kernel_exceeds_sm_resources')

    # The same physical shared banks serve isolated allocations for several resident CTAs.
    # 同一组物理 shared banks 服务多个驻留 CTA 的独立分配。
    def test_shared_reduction(self):
        result = self.timed(reduction(37), '--blocks', 5, '--threads', 37, '--shared-bytes', 256)
        self.assertGreater(result['hardware']['peak_ctas_per_sm'], 1)
        self.assertEqual([word(result['global'], 4*b) for b in range(5)], [sum(range(37))]*5)
        self.assertGreater(sum(s['bank_services'] for s in result['hardware']['shared']), 0)

    def test_shared_conflicts_and_broadcast(self):
        reports = []
        for stride in (4, 128, 0):
            p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=stride)
            p.emit('IMUL', rd=2, ra=0, rb=1).emit('LDS', rd=3, ra=2, mode=4).emit('EXIT')
            reports.append(self.timed(p))
        durations = [events(report, 3, 'retire')[0]-events(report, 3, 'execute')[0] for report in reports]
        self.assertEqual(durations[1] - durations[0], 31)
        self.assertEqual(durations[0], durations[2])
        self.assertEqual(reports[2]['hardware']['shared'][0]['broadcasts'], 31)

    def test_real_cache_and_store_invalidation(self):
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('LDG', rd=1, ra=Z, mode=4)
        p.emit('MOVI', rd=2, imm=71).emit('STG', ra=Z, rb=2, mode=4)
        p.emit('LDG', rd=3, ra=Z, mode=4).emit('EXIT')
        result = self.timed(p, '--threads', 1, images={'global': struct.pack('<I', 12)})
        self.assertEqual([self.registers(result, r)[0] for r in range(4)], [12, 12, 71, 71])
        self.assertEqual(result['hardware']['l1'][0]['hits'], 1)
        self.assertEqual(result['hardware']['dram']['reads'], 2)
        self.assertEqual(word(result['global'], 0), 71)

    def test_subwords_and_generic_shared(self):
        p = Program().emit('LDC', rd=0, ra=Z, mode=1)
        p.emit('CVTA', rd=1, ra=Z, mode=0).emit('ST', ra=1, rb=0, mode=2)
        p.emit('LD', rd=2, ra=1, mode=3).emit('EXIT')
        result = self.timed(p, '--threads', 1, images={'constant': bytes([0x80])})
        self.assertEqual(self.registers(result, 0)[0], 0xFFFFFF80)
        self.assertEqual(self.registers(result, 2)[0], 0xFFFFFF80)
        self.assertEqual(word(result['ctas'][0]['shared'], 0, 2), 0xFF80)

    # Compare many overlapping RAW/WAW/WAR lifetimes to an independent integer interpreter.
    # 将大量重叠的 RAW/WAW/WAR 生命周期与独立整数解释器比较。
    def test_randomized_integer_dependencies(self):
        rng = random.Random(20261029)
        for case in range(12):
            p, values = Program(), [0]*16
            for _ in range(120):
                dst, left, right = [rng.randrange(16) for _ in range(3)]
                opcode = rng.choice(['MOVI', 'IADD', 'ISUB', 'IMUL', 'XOR'])
                if opcode == 'MOVI':
                    values[dst] = rng.randrange(1 << 32)
                    p.emit(opcode, rd=dst, imm=values[dst])
                else:
                    a, b = values[left], values[right]
                    values[dst] = {'IADD': a+b, 'ISUB': a-b, 'IMUL': a*b, 'XOR': a^b}[opcode] & 0xFFFFFFFF
                    p.emit(opcode, rd=dst, ra=left, rb=right)
            result = self.timed(p.emit('EXIT'), '--threads', 1, '--collectors', 1 + case % 4)
            self.assertEqual([self.registers(result, r)[0] for r in range(16)], values)

    def test_explicit_limits_and_memory_errors(self):
        self.timed(Program().emit('LDL', rd=0, ra=Z, imm=16384), error='memory_out_of_bounds')
        self.timed(Program().emit('ATOM', ra=Z, mode=10), error='invalid_atomic_mode')
        self.timed(Program().emit('BRA', imm=0), '--max-cycles', 10, error='cycle_limit_exceeded')
        self.timed(Program().emit('BRA', imm=0), '--max-steps', 5, error='step_limit_exceeded')
        self.timed(Program().emit('LDS', rd=0, ra=Z, imm=256, mode=4), '--shared-bytes', 256,
                   error='memory_out_of_bounds')
        p = Program().emit('LDS', rd=0, ra=Z, imm=65536, guard=NOT_PT).emit('EXIT')
        self.assertEqual(self.timed(p)['ctas'][0]['shared'], [])


# Reuse all control semantics checks under the timing scheduler rather than duplicating them.
# 在时序调度器下复用全部控制语义检查，不复制测试内容。
class TimedControlTest(ExistingControlTests):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', *options, **kwargs)


# Reuse supported architectural cases, including negative diagnostics and cross-warp barriers.
# 复用已接入的架构案例，包括负例诊断和跨 warp 屏障。
class TimedMemoryTest(SimulationCase):
    test_subwords_and_constants = ExistingMemoryTests.test_subwords_and_constants
    test_sparse_addresses = ExistingMemoryTests.test_sparse_zero_normalization_and_high_address
    test_barrier_reuse = ExistingMemoryTests.test_divergent_barrier_and_generation_reuse
    test_early_exit = ExistingMemoryTests.test_early_exit_and_explicit_count
    test_producer_consumer = ExistingMemoryTests.test_fair_producer_consumer_progress
    test_memory_faults = ExistingMemoryTests.test_memory_faults_and_predicated_off_access

    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', '--partitions', 1, *options, **kwargs)

    # Delay the other warp explicitly; issue latency can interleave otherwise adjacent arrivals.
    # 显式延迟另一 warp，否则发射延迟可能让原本相邻的到达发生交错。
    def test_arrival_misuse(self):
        p = Program().emit('S2R', rd=0, imm=2).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('BRA', imm='slow', guard=8)
        p.emit('BAR_ARRIVE', rd=0, imm=64).emit('BAR_ARRIVE', rd=0, imm=64).emit('EXIT')
        p.label('slow')
        for _ in range(24):
            p.emit('NOP')
        p.emit('BAR_ARRIVE', rd=0, imm=64).emit('EXIT')
        result = self.execute(p, '--threads', 64, error='duplicate_barrier_arrival')
        self.assertIn('block=0 sm=0 warp=0 pc=4', result['error'])
        self.execute(Program().emit('BAR_SYNC', rd=16), error='cta_barrier_id_out_of_range')
        self.execute(Program().emit('BAR_SYNC', imm=33), error='invalid_barrier_count')


class TimedScalarTest(SimulationCase):
    test_integer_reference = ExistingScalarTests.test_integer_programs_against_scalar_reference
    test_predicates = ExistingScalarTests.test_predicates_and_zero_register
    test_predicated_exit = ExistingScalarTests.test_predicated_exit_and_partial_predicate_write
    test_comparison_modes = ExistingScalarTests.test_integer_comparison_modes
    test_fused_rounding = ExistingScalarTests.test_floating_operations_and_fused_rounding
    test_errors = ExistingScalarTests.test_errors_are_contextual

    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', *options, **kwargs)


del ExistingControlTests
del ExistingMemoryTests
del ExistingScalarTests

if __name__ == '__main__':
    unittest.main(verbosity=2)

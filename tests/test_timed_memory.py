# Run private memory and RMW contracts through clocked caches and bank resources.
# 通过逐拍缓存和 bank 资源执行私有内存及 RMW 契约。
import random
import unittest
from support import SimulationCase, word
import test_memory as functional
from simt_lab import Program, Z, NOT_PT
from test_async import transaction_program


def cycles(report, pc, action):
    return [event['cycle'] for event in report['hardware']['trace'] if event['pc'] == pc and event['action'] == action]


# Python's unbounded integers provide an independent wrapping and signed-comparison reference.
# Python 无限精度整数提供独立的取模及有符号比较参考。
def atomic_reference(mode, old, value, replacement):
    signed_old = old if old < 1 << 31 else old - (1 << 32)
    signed_value = value if value < 1 << 31 else value - (1 << 32)
    values = [old+value, value, replacement if old == value else old, old & value, old | value,
              old ^ value, min(old, value), max(old, value),
              old if signed_old < signed_value else value, old if signed_old > signed_value else value]
    return values[mode] & 0xFFFFFFFF


def atomic_kernel(space, stride):
    p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=stride)
    p.emit('IMUL', rd=2, ra=0, rb=1).emit('MOVI', rd=3, imm=1)
    return p.emit('ATOM', rd=4, ra=2, rb=3, aux=space).emit('EXIT')


def random_atomics(seed, space):
    rng = random.Random(seed)
    memory, returned, p = [0]*8, [], Program()
    for index in range(48):
        at, mode = rng.randrange(8), rng.randrange(10)
        value = memory[at] if mode == 2 and index % 2 else rng.getrandbits(32)
        replacement = rng.getrandbits(32)
        p.emit('MOVI', rd=1, imm=value).emit('MOVI', rd=2, imm=replacement)
        p.emit('ATOM', rd=3, ra=Z, rb=1, rc=2, imm=at*4, mode=mode, aux=space)
        p.emit('STG', ra=Z, rb=3, imm=2048+index*4, mode=4)
        returned.append(memory[at])
        memory[at] = atomic_reference(mode, memory[at], value, replacement)
    return p.emit('EXIT'), returned, memory


class TimedMemoryContracts(SimulationCase):
    test_spaces = functional.MemoryTest.test_spaces_and_generic_translation
    test_atomic_tickets = functional.MemoryTest.test_global_atomic_tickets_and_shared_reduction
    test_atomic_modes = functional.MemoryTest.test_atomic_compare_exchange_and_modes

    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', '--hardware-trace', *options, **kwargs)


class MemoryPipelineTest(SimulationCase):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', '--hardware-trace', *options, **kwargs)

    def test_local_coalescing_and_cache_reuse(self):
        p = Program().emit('LDL', rd=1, ra=Z, mode=4).emit('LDL', rd=2, ra=Z, mode=4).emit('EXIT')
        result = self.execute(p)
        self.assertEqual(result['hardware']['dram']['reads'], 4)
        self.assertEqual(result['hardware']['l1'][0]['hits'], 4)
        self.assertLess(cycles(result, 1, 'retire')[0]-cycles(result, 1, 'issue')[0],
                        cycles(result, 0, 'retire')[0]-cycles(result, 0, 'issue')[0])

    def test_local_divergent_offsets_generate_more_sectors(self):
        reports = []
        for stride in (0, 4):
            p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=stride)
            p.emit('IMUL', rd=2, ra=0, rb=1).emit('LDL', rd=3, ra=2, mode=4).emit('EXIT')
            reports.append(self.execute(p)['hardware'])
        self.assertEqual([r['dram']['reads'] for r in reports], [4, 32])
        self.assertGreater(reports[1]['cycles'], reports[0]['cycles'])

    def test_local_subwords_and_last_word(self):
        p = Program().emit('MOVI', rd=0, imm=0x8034FF80).emit('STL', ra=Z, rb=0, imm=16380, mode=4)
        for reg, mode, offset in ((1,0,16380), (2,1,16380), (3,2,16382), (4,3,16382), (5,4,16380)):
            p.emit('LDL', rd=reg, ra=Z, imm=offset, mode=mode)
        result = self.execute(p.emit('EXIT'), '--threads', 37)
        for reg, expected in enumerate((128, 0xFFFFFF80, 0x8034, 0xFFFF8034, 0x8034FF80), 1):
            self.assertEqual(self.registers(result, reg), [expected]*32)
            self.assertEqual(self.registers(result, reg, warp=1)[:5], [expected]*5)

    def test_generic_mixed_local_shared_global(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('MOVI', rd=1, imm=1).emit('ISETP', rd=1, ra=0, rb=1)
        p.emit('MOVI', rd=2, imm=0xF1000000, guard=0).emit('MOVI', rd=2, imm=0xF0000000, guard=1)
        p.emit('MOVI', rd=3, imm=11).emit('IADD', rd=3, ra=3, rb=0)
        p.emit('ST', ra=2, rb=3, mode=4).emit('LD', rd=4, ra=2, mode=4).emit('EXIT')
        result = self.execute(p, '--threads', 3)
        self.assertEqual(self.registers(result, 4)[:3], [11, 12, 13])
        self.assertEqual(word(result['ctas'][0]['local'][0], 0), 11)
        self.assertEqual(word(result['ctas'][0]['shared'], 0), 12)
        self.assertEqual(word(result['global'], 0), 13)

    def test_local_slot_reuse_preserves_cta_ownership(self):
        p = Program().emit('LDL', rd=7, ra=Z, mode=4).emit('S2R', rd=0, imm=3)
        p.emit('MOVI', rd=1, imm=1).emit('IADD', rd=0, ra=0, rb=1)
        p.emit('STL', ra=Z, rb=0, mode=4).emit('LDL', rd=2, ra=Z, mode=4).emit('EXIT')
        result = self.execute(p, '--blocks', 7, '--sms', 1, '--threads', 5, '--max-ctas', 1)
        for block in range(7):
            self.assertEqual(self.registers(result, 7, block)[:5], [0]*5)
            self.assertEqual(self.registers(result, 2, block)[:5], [block+1]*5)
            self.assertEqual(word(result['ctas'][block]['local'][4], 0), block+1)

    def test_local_predication_bounds_and_alignment(self):
        for op in ('LDL', 'STL'):
            self.execute(Program().emit(op, ra=Z, imm=16384), error='memory_out_of_bounds')
            self.execute(Program().emit(op, ra=Z, imm=1, mode=4), error='misaligned_memory_access')
            p = Program().emit(op, ra=Z, imm=16384, guard=NOT_PT).emit('EXIT')
            self.assertEqual(self.execute(p)['hardware']['dram']['reads'], 0)

    def test_global_and_shared_random_atomic_modes(self):
        for space in (0, 1):
            for seed in (101, 202, 303):
                p, returned, memory = random_atomics(seed, space)
                result = self.execute(p, '--threads', 1)
                self.assertEqual([word(result['global'], 2048+index*4) for index in range(48)], returned)
                snapshot = result['global'] if space == 0 else result['ctas'][0]['shared']
                self.assertEqual([word(snapshot, index*4) for index in range(8)], memory)

    def test_atomic_banks_and_latency(self):
        for space, stride in ((0, 32), (1, 4)):
            same = self.execute(atomic_kernel(space, 0))
            spread = self.execute(atomic_kernel(space, stride))
            slow = self.execute(atomic_kernel(space, stride), '--atomic-latency', 64)
            self.assertEqual(self.registers(same, 4), list(range(32)))
            self.assertEqual(self.registers(spread, 4), [0]*32)
            self.assertGreater(same['hardware']['cycles'], spread['hardware']['cycles'])
            self.assertGreater(slow['hardware']['cycles'], spread['hardware']['cycles'])

    def test_atomic_queue_backpressure_and_bank_configuration(self):
        p = atomic_kernel(0, 32)
        for banks in (1, 8):
            result = self.execute(p, '--blocks', 6, '--shared-bytes', 256,
                                  '--atomic-depth', 1, '--atomic-banks', banks)
            self.assertEqual(result['hardware']['global_atomic']['completed'], 192)
            self.assertGreater(result['hardware']['global_atomic']['queue_stalls'], 0)
            for lane in range(32):
                tickets = [self.registers(result, 4, block)[lane] for block in range(6)]
                self.assertEqual(sorted(tickets), list(range(6)))

    def test_atomic_dependency_and_independent_alu_overlap(self):
        p = Program().emit('MOVI', rd=0, imm=1).emit('ATOM', rd=1, ra=Z, rb=0)
        p.emit('MOVI', rd=2, imm=77).emit('IADD', rd=3, ra=1, rb=0).emit('EXIT')
        result = self.execute(p)
        self.assertLess(cycles(result, 2, 'retire')[0], cycles(result, 1, 'retire')[0])
        self.assertGreaterEqual(cycles(result, 3, 'issue')[0], cycles(result, 1, 'retire')[0])
        self.assertEqual(self.registers(result, 3), list(range(1, 33)))

    def test_cache_is_invalidated_after_atomic_write(self):
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('MOVI', rd=1, imm=1)
        p.emit('ATOM', rd=2, ra=Z, rb=1).emit('LDG', rd=3, ra=Z, mode=4).emit('EXIT')
        result = self.execute(p)
        self.assertEqual(self.registers(result, 0), [0]*32)
        self.assertEqual(self.registers(result, 3), [32]*32)
        self.assertEqual(result['hardware']['global_atomic']['completed'], 32)

    def test_compare_exchange_uses_collected_replacement(self):
        for space in (0, 1):
            p = Program().emit('MOVI', rd=4, imm=11).emit('ATOM', rd=8, ra=Z, rb=Z, rc=4, mode=2, aux=space)
            p.emit('MOVI', rd=4, imm=99).emit('EXIT')
            result = self.execute(p, '--threads', 1, '--register-banks', 1)
            snapshot = result['global'] if space == 0 else result['ctas'][0]['shared']
            self.assertEqual(word(snapshot, 0), 11)
            self.assertEqual(self.registers(result, 8)[0], 0)

    def test_reduction_and_masked_atomics(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16)
        p.emit('ISETP', rd=0, ra=0, rb=1, mode=2).emit('MOVI', rd=2, imm=1)
        p.emit('RED', ra=Z, rb=2, guard=0).emit('ATOM', rd=3, ra=Z, imm=1, guard=NOT_PT)
        result = self.execute(p.emit('EXIT'))
        self.assertEqual(word(result['global'], 0), 16)
        self.assertEqual(result['hardware']['global_atomic']['completed'], 16)

    def test_atomic_errors_and_opaque_barriers(self):
        for kwargs, error in (({'mode':10}, 'invalid_atomic_mode'), ({'aux':2}, 'invalid_atomic_space'),
                              ({'imm':1}, 'misaligned_memory_access')):
            self.execute(Program().emit('ATOM', ra=Z, **kwargs), error=error)
        self.execute(transaction_program().emit('RED', ra=1, rb=Z, aux=1), error='write_overlaps_mbarrier')
        for option, value in (('--atomic-latency',0), ('--atomic-banks',0), ('--atomic-depth',65)):
            self.execute(Program().emit('EXIT'), option, value, error='invalid_atomic_config')


if __name__ == '__main__':
    unittest.main(verbosity=2)

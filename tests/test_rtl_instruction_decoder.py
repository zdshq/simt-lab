# Validate actual decoded masks and fields at ISA and persistent-host boundaries.
# 在 ISA 与持久主机边界验证实际译码掩码和字段。
import unittest
from support import SimulationCase
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


def check_decoders(test, hardware):
    units = hardware['instruction_decoders']
    test.assertTrue(units)
    schedulers = {(u['sm'], u['partition']): u for u in hardware['warp_schedulers']}
    for unit in units:
        test.assertEqual((unit['backend'], unit['instruction_bits'], unit['source_ports'], unit['storage_bits']), ('rtl', 128, 3, 0))
        test.assertEqual(unit['cycles'], hardware['cycles'])
        test.assertEqual(unit['encoding_errors']+unit['operand_errors'], 0)
        scheduler = schedulers[unit['sm'], unit['partition']]
        test.assertEqual(unit['inspected'], scheduler['probes'])
        test.assertEqual(unit['operands'], unit['inspected']-scheduler['guard_stalls'])
        test.assertEqual(unit['accepted'], scheduler['issued'])
        test.assertLessEqual(unit['inspected'], unit['cycles'])
        test.assertLessEqual(unit['sources'], unit['operands']*3)
    test.assertEqual(sum(u['accepted'] for u in units), hardware['issued'])


class InstructionDecoding(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        result = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', '--scoreboard-rtl',
                                 '--hardware-trace', *options, **kwargs)
        if kwargs.get('error') is None: check_decoders(self, result['hardware'])
        return result

    def test_collective_and_guarded_source_masks(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16).emit('ISETP', rd=0, ra=0, rb=1, mode=2)
        p.emit('SHFL', rd=3, ra=0, rb=0, imm=0xffff, guard=0)
        p.emit('ACTIVEMASK', rd=4, guard=0).emit('VOTE', rd=5, ra=0, imm=0xffffffff)
        p.emit('IADD', rd=6, ra=0, rb=0, guard=0).emit('EXIT')
        for parts in (1, 2, 4, 8):
            result = self.execute(p, '--sms', 1, '--threads', 32, '--partitions', parts)
            self.assertEqual(self.registers(result, 3), list(range(16))+[0]*16)
            self.assertEqual(self.registers(result, 4), [0xffffffff]*16+[0]*16)
            self.assertEqual(self.registers(result, 5), [0xffff]*32)
            self.assertEqual(self.registers(result, 6), [i*2 for i in range(16)]+[0]*16)

    def test_three_sources_highest_register_and_zero(self):
        p = Program().emit('MOVI', rd=62, imm=7).emit('MOVI', rd=1, imm=3).emit('MOVI', rd=2, imm=5)
        p.emit('IMAD', rd=3, ra=62, rb=1, rc=2).emit('IMAD', rd=4, ra=62, rb=62, rc=62)
        p.emit('IMAD', rd=5, ra=Z, rb=62, rc=1).emit('MOVI', rd=Z, imm=123).emit('EXIT')
        result = self.execute(p, '--blocks', 7, '--threads', 37, '--max-ctas', 1)
        for cta in range(7):
            for warp, count in ((0, 32), (1, 5)):
                for reg, expected in ((3, 26), (4, 56), (5, 3), (63, 0)):
                    self.assertEqual(self.registers(result, reg, block=cta, warp=warp)[:count], [expected]*count)

    def test_structural_errors_and_inactive_sources(self):
        cases = [(Program().emit('MOVI', rd=64, imm=1, guard=15), 'register_out_of_range'),
                 (Program().emit('MOV', rd=0, ra=255), 'register_out_of_range'),
                 (Program().emit('ISETP', rd=7, ra=Z, rb=Z, guard=15), 'predicate_out_of_range'),
                 (Program().emit('VOTE', rd=0, ra=8, imm=1, guard=15), 'predicate_out_of_range')]
        for program, error in cases:
            report = self.execute(program.emit('EXIT'), '--threads', 1, error=error)
            self.assertIn('pc=0', report['error'])
        # Source collection skips inactive lanes; downstream descriptor validation remains independent.
        # 源收集跳过非活动 lane；下游描述符校验仍独立执行。
        p = Program().emit('MOV', rd=0, ra=255, guard=15).emit('EXIT')
        self.execute(p, '--threads', 1, error='integer_descriptor')
        p = Program().emit('MOV', rd=0, ra=62, guard=15).emit('IMAD', rd=1, ra=62, rb=62, rc=62, guard=15).emit('EXIT')
        result = self.execute(p, '--threads', 1)
        self.assertEqual([self.registers(result, reg)[0] for reg in (0, 1)], [0, 0])
        self.assertEqual(sum(u['sources'] for u in result['hardware']['instruction_decoders']), 0)

    def test_divergent_pc_groups_and_control_field(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16).emit('ISETP', rd=0, ra=0, rb=1, mode=2)
        p.emit('BRA', imm='taken', guard=0).emit('MOVI', rd=2, imm=222, control=0x1fffff).emit('BRA', imm='join')
        p.label('taken').emit('MOVI', rd=2, imm=111, control=0x12345)
        p.label('join').emit('IMAD', rd=3, ra=2, rb=Z, rc=2).emit('EXIT')
        result = self.execute(p, '--threads', 32, '--blocks', 5)
        for cta in range(5): self.assertEqual(self.registers(result, 3, block=cta), [111]*16+[222]*16)


class DecoderHost(HostCase):
    def test_queries_and_failed_decode_then_graph_replay(self):
        device = self.device(timing=True, register_rtl=True, collector_rtl=True, scoreboard_rtl=True, hardware_trace=True)
        bad = device.load(Program().emit('MOVI', rd=255, imm=1).emit('EXIT')).launch(threads=1, stream=1)
        with self.assertRaisesRegex(DeviceError, 'register_out_of_range'): bad.wait()
        stopped = bad.query(); self.assertEqual(sum(u['operand_errors'] for u in stopped['hardware']['instruction_decoders']), 1)
        for _ in range(20): self.assertEqual(bad.query(), stopped)
        module = device.load(Program().emit('MOVI', rd=0, imm=17).emit('EXIT'))
        graph = device.graph(); compute = graph.add(module.operation(threads=1)); executable = graph.instantiate()
        for _ in range(2):
            replay = executable.launch(stream=2); replay.wait()
            check_decoders(self, replay.node_event(compute).query()['hardware'])


if __name__ == '__main__':
    unittest.main(verbosity=2)

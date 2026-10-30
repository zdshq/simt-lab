# Exercise physical predicate values through comparisons, issue guards, collectives and reused warp slots.
# 通过比较、发射谓词、集合操作和复用 warp 槽验证物理谓词值。
import unittest
from support import SimulationCase
from simt_lab import Program, Z, NOT_PT


class PredicatePipeline(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--hardware-trace', *options, **kwargs)
        hardware = report['hardware']
        self.assertTrue(hardware['predicate_files'])
        self.assertEqual(sum(unit['guard_reads'] for unit in hardware['predicate_files']), hardware['issued'])
        for unit in hardware['predicate_files']:
            self.assertEqual((unit['backend'], unit['read_ports'], unit['write_ports']), ('rtl', 3, 1))
            self.assertEqual(unit['clocks'], hardware['cycles'])
            self.assertEqual(unit['data_bits'], unit['slots']*224)
            self.assertEqual(unit['valid_bits'], unit['slots']*7)
        return report

    def backends(self):
        return ((), ('--collector-rtl',))

    # A partial overwrite of P1 must preserve its other lanes before a full-warp ballot reads it.
    # P1 的部分覆盖必须保留其他 lane，随后由全 warp 投票读取。
    def test_comparison_masked_overwrite_and_vote(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16)
        p.emit('ISETP', rd=0, ra=0, rb=1, mode=2).emit('ISETP', rd=1, ra=Z, rb=Z)
        p.emit('ISETP', rd=1, ra=Z, rb=Z, mode=1, guard=0)
        p.emit('VOTE', rd=2, ra=1, imm=0xffffffff).emit('MOVI', rd=3, imm=91, guard=1)
        p.emit('ELECT', rd=2, imm=0xffffffff).emit('MOVI', rd=4, imm=73, guard=2).emit('EXIT')
        for backend in self.backends():
            report = self.execute(p, *backend)
            self.assertEqual(self.registers(report, 2), [0xffff0000]*32)
            self.assertEqual(self.registers(report, 3), [0]*16+[91]*16)
            self.assertEqual(self.registers(report, 4), [73]+[0]*31)
            units = report['hardware']['predicate_files']
            self.assertEqual(sum(unit['writes'] for unit in units), 4)
            self.assertEqual(sum(unit['source_reads'] for unit in units), 1)

    def test_floating_predicate_same_cycle_issue(self):
        p = Program().emit('MOVI', rd=0, imm=0x3f800000).emit('MOVI', rd=1, imm=0x40000000)
        p.emit('FSETP', rd=6, ra=0, rb=1, mode=2).emit('MOVI', rd=2, imm=123, guard=6)
        p.emit('MOVI', rd=3, imm=999, guard=14).emit('EXIT')
        for backend in self.backends():
            report = self.execute(p, *backend, '--threads', 37)
            for warp, count in ((0, 32), (1, 5)):
                self.assertEqual(self.registers(report, 2, warp=warp), [123]*count+[0]*(32-count))
                self.assertEqual(self.registers(report, 3, warp=warp), [0]*32)
            trace = report['hardware']['trace']
            for warp in (0, 1):
                retired = [event['cycle'] for event in trace if event['warp'] == warp and event['pc'] == 2 and event['action'] == 'retire']
                issued = [event['cycle'] for event in trace if event['warp'] == warp and event['pc'] == 3 and event['action'] == 'issue']
                self.assertEqual(issued, retired)

    def test_slot_reuse_geometry_and_constant_predicate(self):
        p = Program().emit('MOVI', rd=0, imm=999, guard=0).emit('S2R', rd=1, imm=2)
        p.emit('ISETP', rd=0, ra=Z, rb=Z).emit('MOVI', rd=2, imm=17, guard=0)
        p.emit('MOVI', rd=3, imm=9, guard=NOT_PT).emit('EXIT')
        for backend in self.backends():
            for partitions in (1, 2, 4, 8):
                report = self.execute(p, *backend, '--partitions', partitions, '--blocks', 5, '--threads', 37, '--max-ctas', 1)
                for block in range(5):
                    for warp, count in ((0, 32), (1, 5)):
                        self.assertEqual(self.registers(report, 0, block, warp), [0]*32)
                        self.assertEqual(self.registers(report, 2, block, warp), [17]*count+[0]*(32-count))
                        self.assertEqual(self.registers(report, 3, block, warp), [0]*32)
                self.assertEqual(sum(unit['writes'] for unit in report['hardware']['predicate_files']), 10)


if __name__ == '__main__':
    unittest.main(verbosity=2)

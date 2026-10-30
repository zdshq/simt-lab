# Compare connected collective circuits with the independent functional ISA and inspect actual handshakes.
# 将直连集合电路与独立功能 ISA 比较，并检查实际握手。
import unittest
from support import SimulationCase
from simt_lab import Program, Z, NOT_PT


class CollectivePipeline(SimulationCase):
    execution_timeout = 120

    def compare(self, program, *options, **kwargs):
        reference = self.execute(program, *options, **kwargs)
        actual = self.execute(program, '--timing', '--register-rtl', '--collector-rtl', '--hardware-trace', *options, **kwargs)
        if kwargs.get('error') is not None: return actual
        self.assertEqual(actual['ctas'], reference['ctas'])
        hardware = actual['hardware']
        units = hardware['collective_units']
        self.assertTrue(units)
        for unit in units:
            self.assertEqual((unit['backend'], unit['entries'], unit['pipeline_stages']), ('rtl', 1, 1))
            self.assertEqual(unit['accepted'], unit['completed'])
            self.assertEqual(unit['clocks'], hardware['cycles'])
            self.assertEqual(unit['occupancy'], 0)
        return actual

    def test_shuffle_in_place_and_votes(self):
        p = Program().emit('S2R', rd=1, imm=0).emit('MOVI', rd=2, imm=31).emit('ISUB', rd=2, ra=2, rb=1)
        p.emit('SHFL', rd=1, ra=1, rb=2, imm=0xffffffff).emit('MOVI', rd=3, imm=16)
        p.emit('ISETP', rd=0, ra=1, rb=3, mode=2)
        for mode in range(4): p.emit('VOTE', rd=4+mode, ra=0, mode=mode, imm=0xffffffff)
        p.emit('ELECT', rd=1, imm=0xffffffff).emit('MOVI', rd=8, imm=77, guard=1)
        p.emit('ACTIVEMASK', rd=9).emit('EXIT')
        for partitions in (1, 2, 4, 8):
            result = self.compare(p, '--threads', 64, '--blocks', 3, '--partitions', partitions)
            self.assertEqual(sum(u['accepted'] for u in result['hardware']['collective_units']), 42)
            self.assertEqual(sum(u['vote_reads'] for u in result['hardware']['collective_units']), 24)

    def test_subsets_zero_sources_and_inactive_faults(self):
        p = Program().emit('MOVI', rd=1, imm=73).emit('MOVI', rd=2, imm=0)
        p.emit('SHFL', rd=1, ra=Z, rb=2, imm=0x55555555)
        p.emit('VOTE', rd=3, ra=7, imm=0x55555555).emit('ELECT', rd=0, imm=0x80000000)
        p.emit('ACTIVEMASK', rd=4, guard=0)
        p.emit('VOTE', rd=0, ra=1, imm=0, mode=255, guard=NOT_PT)
        p.emit('SHFL', rd=0, ra=1, rb=2, imm=0, guard=NOT_PT)
        p.emit('ELECT', rd=0, imm=0, guard=NOT_PT)
        self.compare(p.emit('EXIT'))

    def test_partial_warps_and_slot_reuse(self):
        p = Program().emit('ACTIVEMASK', rd=1).emit('VOTE', rd=2, ra=7, imm=31)
        p.emit('ELECT', rd=0, imm=31).emit('SHFL', rd=3, ra=1, rb=Z, imm=31).emit('EXIT')
        self.compare(p, '--threads', 37, '--blocks', 7, '--max-ctas', 1)

    def test_faults_preserve_isa_diagnostics(self):
        programs = [(Program().emit('VOTE', rd=1, ra=0, imm=0), 'empty_collective_members'),
                    (Program().emit('VOTE', rd=1, ra=0, imm=1, mode=4), 'invalid_vote_mode'),
                    (Program().emit('SHFL', rd=1, ra=Z, rb=Z, imm=2), 'shuffle_source_not_member'),
                    (Program().emit('ELECT', rd=0, imm=0xffffffff), 'membermask_not_subset')]
        for p, error in programs:
            self.compare(p.emit('EXIT'), '--threads', 5, error=error)


if __name__ == '__main__':
    unittest.main(verbosity=2)

# Verify actual ISA dispatch, physical floating stages, completion storage, and exceptional behavior.
# 验证实际 ISA 分派、物理浮点流水级、完成存储与异常行为。
import math
import struct
import unittest
from support import SimulationCase
from simt_lab import Program, Z, NOT_PT


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


class FloatingPipeline(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', '--hardware-trace', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            self.assertTrue(hardware['floating_units'])
            for unit in hardware['floating_units']:
                self.assertEqual((unit['backend'], unit['pipeline_stages'], unit['entries']), ('rtl', 4, 32))
                self.assertEqual(unit['occupancy'], 0)
                self.assertEqual(unit['accepted'], unit['completed'])
                self.assertEqual(unit['clocks'], hardware['cycles'])
        return report

    def test_physical_latency_and_dependent_results(self):
        p = Program().emit('MOVI', rd=1, imm=bits(1)).emit('MOVI', rd=2, imm=bits(2)).emit('MOVI', rd=3, imm=bits(3))
        p.emit('FFMA', rd=4, ra=1, rb=2, rc=3).emit('FADD', rd=5, ra=4, rb=1).emit('FMUL', rd=6, ra=5, rb=2)
        p.emit('FSETP', rd=0, ra=6, rb=3, mode=2).emit('MOVI', rd=7, imm=0xfffffffd)
        p.emit('I2F', rd=8, ra=7).emit('F2I', rd=9, ra=8).emit('EXIT')
        for latency in (4, 6, 100):
            result = self.execute(p, '--threads', 1, '--fp-latency', latency)
            self.assertEqual([self.registers(result, r)[0] for r in (4, 5, 6, 8, 9)], [bits(5), bits(6), bits(12), bits(-3), 0xfffffffd])
            hardware = result['hardware']
            self.assertEqual(sum(unit['accepted'] for unit in hardware['floating_units']), 6)
            starts = {event['instruction']: event['cycle'] for event in hardware['trace'] if event['action'] == 'execute'}
            for event in hardware['trace']:
                if event['action'] == 'retire' and event['pc'] in (3, 4, 5, 6, 8, 9):
                    self.assertEqual(event['cycle']-starts[event['instruction']], latency)

    def test_checked_conversion_and_inactive_lanes(self):
        for value in (0x4f000000, 0xcf000001, 0x7f800000, 0xff800000, 0x7fc00000):
            p = Program().emit('MOVI', rd=1, imm=value).emit('F2I', rd=2, ra=1).emit('EXIT')
            self.execute(p, '--threads', 1, error='float_to_integer_out_of_range')
        p = Program().emit('MOVI', rd=1, imm=0x7f800000).emit('F2I', rd=2, ra=1, guard=NOT_PT)
        p.emit('FSETP', rd=0, ra=1, rb=Z, mode=255, guard=NOT_PT).emit('EXIT')
        result = self.execute(p, '--threads', 1)
        self.assertEqual(self.registers(result, 2)[0], 0)
        self.execute(Program().emit('FSETP', rd=0, ra=Z, rb=Z, mode=4).emit('EXIT'), '--threads', 1, error='invalid_float_comparison')

    def conversion_program(self, opcode):
        return (Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=4).emit('IMUL', rd=2, ra=0, rb=1)
                .emit('LDC', rd=3, ra=2, mode=4).emit(opcode, rd=4, ra=3).emit('EXIT'))

    def test_lane_conversion_boundaries(self):
        integers = ([0, 1, -1, 16777215, 16777217, 16777219, 2147483647, -2147483648]*4)
        result = self.execute(self.conversion_program('I2F'), images={'constant': struct.pack('<32i', *integers)})
        self.assertEqual(self.registers(result, 4), [bits(value) for value in integers])
        floats = [0.0, -0.0, 1.5, -1.5, 0.999, -0.999, 2147483520.0, -2147483648.0]*4
        result = self.execute(self.conversion_program('F2I'), images={'constant': struct.pack('<32f', *floats)})
        self.assertEqual(self.registers(result, 4), [math.trunc(value) & 0xffffffff for value in floats])

    def test_floating_reservations_backpressure_issue(self):
        p = Program()
        for register in range(40):
            p.emit('FADD', rd=register, ra=Z, rb=Z)
        result = self.execute(p.emit('EXIT'), '--threads', 1, '--fp-latency', 100)
        hardware = result['hardware']
        self.assertGreater(hardware['completion_stalls'], 0)
        self.assertEqual(sum(unit['accepted'] for unit in hardware['floating_units']), 40)
        active, peak = 0, 0
        for event in hardware['trace']:
            if event['action'] == 'issue':
                active += 1
                peak = max(peak, active)
            elif event['action'] == 'retire':
                active -= 1
            self.assertLessEqual(active, 32)
        self.assertEqual((active, peak), (0, 32))

    def test_pipeline_latency_cannot_skip_registers(self):
        program = Program().emit('FADD', rd=0, ra=Z, rb=Z).emit('EXIT')
        for latency in (1, 2, 3):
            self.execute(program, '--fp-latency', latency, error='floating_rtl_requires_latency_at_least_4')
        software = super().execute(program, '--timing', '--fp-latency', 1)
        self.assertEqual(self.registers(software, 0)[0], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)

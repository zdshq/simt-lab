# Keep TMEM data, column ownership and final retirement observable through real ISA programs.
# 通过实际 ISA 程序观察 TMEM 数据、列归属和最终退休。
import unittest
from support import SimulationCase, word
from simt_lab import Program, Z


class TensorMemoryRtl(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', *options, **kwargs)
        if kwargs.get('error'):
            return report
        for state in report['hardware']['tensor']:
            self.assertEqual(state['tmem_backend'], 'rtl')
            self.assertEqual(state['tmem_bytes'], 131072)
            self.assertEqual(state['tmem_request_slots'], 8)
            self.assertEqual(state['tmem_clocks'], report['hardware']['cycles'])
            self.assertEqual(state['tmem_accepted'], state['tmem_consumed'])
            self.assertEqual(state['tmem_allocated_columns'], state['tmem_freed_columns']+state['tmem_reclaimed_columns'])
            for field in ('tmem_outstanding', 'tmem_pending', 'tmem_used_columns'):
                self.assertEqual(state[field], 0)
        return report

    def test_same_logical_columns_isolate_ctas_and_keep_snapshots(self):
        p = Program().emit('TMEM_ALLOC', rd=0, imm=8).emit('S2R', rd=1, imm=3)
        p.emit('MOVI', rd=2, imm=100).emit('IADD', rd=1, ra=1, rb=2)
        p.emit('STT', ra=Z, rb=Z, rc=1).emit('LDT', rd=3, ra=Z, rb=Z).emit('EXIT')
        result = self.execute(p, '--threads', 1, '--sms', 1, '--blocks', 9, '--shared-bytes', 256,
                              '--max-ctas', 2, '--tmem-columns', 16)
        for block in range(9):
            self.assertEqual(self.registers(result, 0, block)[0], 0)
            self.assertEqual(self.registers(result, 3, block)[0], block+100)
            self.assertEqual(word(result['ctas'][block]['tmem'], 0), block+100)
        self.assertEqual(result['hardware']['tensor'][0]['tmem_peak_columns'], 16)

    def test_free_reallocate_zeroes_actual_sram(self):
        p = Program().emit('TMEM_ALLOC', rd=0, imm=8).emit('MOVI', rd=1, imm=0x12345678)
        p.emit('STT', ra=Z, rb=Z, rc=1).emit('TMEM_FREE', ra=0, imm=8)
        p.emit('TMEM_ALLOC', rd=0, imm=8).emit('LDT', rd=2, ra=Z, rb=Z).emit('EXIT')
        result = self.execute(p, '--threads', 1, '--blocks', 3, '--sms', 1, '--tmem-columns', 8)
        for block in range(3):
            self.assertEqual(self.registers(result, 2, block)[0], 0)
        state = result['hardware']['tensor'][0]
        self.assertEqual((state['tmem_allocated_columns'], state['tmem_freed_columns'], state['tmem_reclaimed_columns']), (48, 24, 24))

    def test_highest_word_and_full_capacity(self):
        p = Program().emit('TMEM_ALLOC', rd=0, imm=256).emit('MOVI', rd=1, imm=127)
        p.emit('MOVI', rd=2, imm=255).emit('MOVI', rd=3, imm=0xfedcba98)
        p.emit('STT', ra=1, rb=2, rc=3).emit('LDT', rd=4, ra=1, rb=2).emit('EXIT')
        result = self.execute(p, '--threads', 1, '--sms', 2, '--blocks', 4)
        for block in range(4):
            self.assertEqual(self.registers(result, 4, block)[0], 0xfedcba98)
            self.assertEqual(word(result['ctas'][block]['tmem'], 131068), 0xfedcba98)
        self.assertEqual([state['tmem_peak_columns'] for state in result['hardware']['tensor']], [256, 256])


if __name__ == '__main__':
    unittest.main()

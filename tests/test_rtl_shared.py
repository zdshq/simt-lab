# Physical shared regions must preserve data across CTA reuse, DMA publication and Tensor consumers.
# 物理 shared 区域必须在 CTA 复用、DMA 发布和 Tensor 消费中保持数据正确。
import unittest
from support import SimulationCase, word
from simt_lab import Program, Z
from test_timed_async import copy_setup, copy_command, finish_copy, shared_bytes


class SharedRtl(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', *options, **kwargs)
        if not kwargs.get('error'):
            for shared in report['hardware']['shared']:
                self.assertEqual(shared['backend'], 'rtl')
                self.assertEqual(shared['outstanding'], 0)
                self.assertEqual(shared['accepted'], shared['consumed'])
                self.assertEqual(shared['clocks'], report['hardware']['cycles'])
        return report

    def test_reused_cta_regions_are_zero_and_keep_retired_snapshots(self):
        p = Program().emit('LDS', rd=7, ra=Z, mode=4).emit('S2R', rd=0, imm=3)
        p.emit('MOVI', rd=1, imm=1).emit('IADD', rd=0, ra=0, rb=1)
        p.emit('STS', ra=Z, rb=0, mode=4).emit('LDS', rd=2, ra=Z, mode=4).emit('EXIT')
        result = self.execute(p, '--sms', 1, '--blocks', 9, '--threads', 1, '--shared-bytes', 32768)
        self.assertEqual(result['hardware']['peak_ctas_per_sm'], 2)
        for block in range(9):
            self.assertEqual(self.registers(result, 7, block)[0], 0)
            self.assertEqual(self.registers(result, 2, block)[0], block+1)
            self.assertEqual(word(result['ctas'][block]['shared'], 0), block+1)

    def test_partial_final_word_and_full_64_kib(self):
        p = Program().emit('MOVI', rd=0, imm=0xab).emit('STS', ra=Z, rb=0, imm=65535, mode=0)
        p.emit('LDS', rd=1, ra=Z, imm=65532, mode=4).emit('EXIT')
        result = self.execute(p, '--threads', 1, '--sms', 1, '--blocks', 3)
        for block in range(3):
            self.assertEqual(self.registers(result, 1, block)[0], 0xab000000)
            self.assertEqual(word(result['ctas'][block]['shared'], 65532), 0xab000000)

    def test_dma_produces_actual_shared_loads_across_cta_reuse(self):
        p = copy_setup(129, source=65, destination=8193)
        p.emit('S2R', rd=8, imm=3).emit('MOVI', rd=9, imm=256).emit('IMAD', rd=4, ra=8, rb=9, rc=4)
        copy_command(p, 129); p.emit('MB_WAIT', ra=1, rb=3).emit('LDS', rd=10, ra=2, mode=0).emit('EXIT')
        data = bytes((index*71+9)%251 for index in range(1600))
        result = self.execute(p, '--threads', 1, '--sms', 1, '--blocks', 6, '--shared-bytes', 16384,
                              '--async-fragments', 1, images={'global': data})
        for block in range(6):
            offset = 65+block*256
            self.assertEqual(shared_bytes(result, 8193, 129, block), data[offset:offset+129])
            self.assertEqual(self.registers(result, 10, block)[0], data[offset])


if __name__ == '__main__':
    unittest.main()

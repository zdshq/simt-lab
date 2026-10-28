# Verify deferred visibility and transaction completion independently of CTA barriers.
# 验证延迟可见性与事务完成，区分它们和 CTA 屏障。
import struct
import unittest
from support import SimulationCase, word
from tensor_support import tensor_program
from simt_lab import Program, Z, f32
from simt_lab.tensor import Image, MatrixDescriptor, pack_values


# Place the opaque transaction record away from the payload and descriptors.
# 将不透明事务记录放在载荷和描述符以外的位置。
def transaction_program(size=4):
    p = Program().emit('S2R',rd=0,imm=1).emit('ISETP',rd=0,ra=0,rb=Z)
    p.emit('MOVI',rd=1,imm=1024).emit('MOVI',rd=2,imm=128)
    p.emit('MB_INIT',ra=1,imm=1,guard=0).emit('MB_EXPECT',ra=1,imm=size,guard=0)
    return p.emit('MB_ARRIVE',rd=3,ra=1,guard=0)


class AsyncTest(SimulationCase):
    # A load before completion sees old bytes; a phase wait must see completed bytes.
    # 完成前读取看到旧字节；等待阶段完成后必须看到新字节。
    def test_deferred_copy_and_wait(self):
        p = transaction_program().emit('CP_ASYNC',ra=Z,rb=2,rc=1,imm=4,guard=0)
        p.emit('LDS',rd=4,ra=2,mode=4).emit('MB_WAIT',ra=1,rb=3)
        p.emit('LDS',rd=5,ra=2,mode=4).emit('EXIT')
        result = self.execute(p,images={'global':struct.pack('<I',0x12345678)})
        self.assertEqual(self.registers(result,4),[0]*32)
        self.assertEqual(self.registers(result,5),[0x12345678]*32)
        self.assertEqual(word(result['ctas'][0]['shared'],1028),1)
        self.assertEqual(word(result['ctas'][0]['shared'],1040),0)

    # A reused barrier returns a new token and waits for the new payload.
    # 复用屏障会返回新令牌，并等待新的载荷。
    def test_repeated_transaction_phases(self):
        p = transaction_program().emit('CP_ASYNC',ra=Z,rb=2,rc=1,imm=4,guard=0)
        p.emit('MB_WAIT',ra=1,rb=3).emit('MOVI',rd=4,imm=4)
        p.emit('MB_EXPECT',ra=1,imm=4,guard=0).emit('MB_ARRIVE',rd=3,ra=1,guard=0)
        p.emit('SHFL',rd=3,ra=3,rb=Z,imm=0xFFFFFFFF)
        p.emit('CP_ASYNC',ra=4,rb=2,rc=1,imm=4,guard=0).emit('MB_WAIT',ra=1,rb=3)
        p.emit('LDS',rd=5,ra=2,mode=4).emit('EXIT')
        result = self.execute(p,images={'global':struct.pack('<2I',7,99)})
        self.assertEqual(self.registers(result,3),[1]*32)
        self.assertEqual(self.registers(result,5),[99]*32)
        self.assertEqual(word(result['ctas'][0]['shared'],1028),2)

    # A CTA remains resident until already-issued payloads finish, even after EXIT.
    # 即使执行了 EXIT，CTA 仍驻留到已经发起的载荷完成。
    def test_exit_drains_outstanding_copy(self):
        p = transaction_program().emit('CP_ASYNC',ra=Z,rb=2,rc=1,imm=4,guard=0).emit('EXIT')
        result = self.execute(p,'--blocks',5,images={'global':struct.pack('<I',91)})
        self.assertEqual(result['blocks_completed'],5)
        self.assertTrue(all(word(cta['shared'],128)==91 for cta in result['ctas']))

    # A copy pipeline feeds shared operands into an actual matrix instruction.
    # 搬运流水线将 shared 操作数送入实际矩阵指令。
    def test_async_shared_matrix_pipeline_across_warps(self):
        image = Image()
        image.append(pack_values([2,3,5,7],0))
        d = MatrixDescriptor(0,1,1,2,128,136,space=0)
        p = tensor_program(d).emit('MOVI',rd=1,imm=1024).emit('MOVI',rd=2,imm=128)
        p.emit('MB_INIT',ra=1,imm=1,guard=0).emit('MB_EXPECT',ra=1,imm=16,guard=0)
        p.emit('MB_ARRIVE',rd=3,ra=1,guard=0).emit('CP_ASYNC',ra=Z,rb=2,rc=1,imm=16,guard=0)
        p.emit('BAR_SYNC',rd=0).emit('MB_WAIT',ra=1,rb=3)
        p.emit('MMA',ra=Z,guard=0).emit('BAR_SYNC',rd=0).emit('LDT',rd=4,ra=Z,rb=Z).emit('EXIT')
        for policy in ('rr','min','max'):
            result = self.execute(p,'--threads',64,'--blocks',3,'--warp-policy',policy,
                                  images={'global':bytes(image.data)})
            for b in range(3):
                for wi in range(2):
                    self.assertEqual(self.registers(result,4,b,wi),[f32(31)]*32)

    # Missing payload bytes cannot be hidden by arrival completion alone.
    # 单纯完成线程到达不能掩盖尚未完成的载荷字节。
    def test_missing_transaction_deadlocks(self):
        p = transaction_program(8).emit('CP_ASYNC',ra=Z,rb=2,rc=1,imm=4,guard=0)
        report = self.execute(p.emit('MB_WAIT',ra=1,rb=3).emit('EXIT'),error='deadlock_no_runnable_group')
        self.assertIn('wait=mbarrier token=1024',report['error'])
        self.execute(transaction_program().emit('EXIT'),error='incomplete_async_transaction')

    # Invalid tokens and byte accounting fail before acknowledging a transaction.
    # 无效令牌与字节计数在确认事务完成之前失败。
    def test_invalid_transaction_usage(self):
        self.execute(Program().emit('MB_WAIT',ra=Z,rb=Z),error='mbarrier_uninitialized')
        p = transaction_program().emit('MOVI',rd=3,imm=2).emit('MB_WAIT',ra=1,rb=3)
        self.execute(p,error='mbarrier_future_phase')
        p = transaction_program().emit('CP_ASYNC',ra=Z,rb=2,rc=1,imm=8,guard=0)
        self.execute(p,error='async_exceeds_expected_bytes')
        p = transaction_program().emit('STS',ra=1,rb=Z,mode=4,guard=0)
        self.execute(p,error='write_overlaps_mbarrier')
        p = transaction_program().emit('CP_ASYNC',ra=Z,rb=1,rc=1,imm=4,guard=0)
        self.execute(p,error='write_overlaps_mbarrier')
        self.execute(Program().emit('MB_INIT',ra=Z,imm=1),error='async_single_issuer_required')


if __name__ == '__main__':
    unittest.main(verbosity=2)

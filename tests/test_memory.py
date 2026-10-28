# Validate memory ownership and synchronization through executable kernels.
# 通过可执行 kernel 验证内存归属与同步。
import struct
import unittest
from support import SimulationCase, word
from simt_lab import Program, Z, NOT_PT

POLICIES = ('rr', 'min', 'max')


# Address one shared word per thread and identify thread zero.
# 为每个线程定位一个 shared 字，并识别零号线程。
def thread_addresses():
    p = Program().emit('S2R', rd=0, imm=1).emit('MOVI', rd=1, imm=4)
    return p.emit('IMUL', rd=2, ra=0, rb=1).emit('ISETP', rd=0, ra=0, rb=Z)


# Let thread zero sum the published values after all threads arrive.
# 所有线程到达后，由零号线程累加已发布的数据。
def reduction(threads):
    p = thread_addresses().emit('STS', ra=2, rb=0, mode=4).emit('BAR_SYNC', rd=0)
    for thread in range(threads):
        p.emit('LDS', rd=3, ra=Z, imm=4*thread, mode=4, guard=0)
        p.emit('IADD', rd=4, ra=4, rb=3, guard=0)
    p.emit('S2R', rd=5, imm=3).emit('IMUL', rd=5, ra=5, rb=1)
    return p.emit('STG', ra=5, rb=4, mode=4, guard=0).emit('EXIT')


class MemoryTest(SimulationCase):
    # Check both extension rules and little-endian host image loading.
    # 检查扩展规则与小端宿主镜像装载。
    def test_subwords_and_constants(self):
        image = bytes.fromhex('80 ff 34 80 78 56 34 12')
        p = Program()
        for reg, mode, offset in [(0,0,0), (1,1,0), (2,2,2), (3,3,2), (4,4,4)]:
            p.emit('LDC', rd=reg, ra=Z, imm=offset, mode=mode)
        p.emit('STG', ra=Z, rb=4, mode=2).emit('STG', ra=Z, rb=3, imm=3, mode=0)
        result = self.execute(p.emit('EXIT'), '--threads', 1, images={'constant': image})
        expected = [128, 0xFFFFFF80, 0x8034, 0xFFFF8034, 0x12345678]
        self.assertEqual([self.registers(result, r)[0] for r in range(5)], expected)
        self.assertEqual(word(result['global'], 0), 0x34005678)
        self.assertEqual(word(result['constant'], 4), 0x12345678)

    # Shared state is isolated between CTAs; local state is isolated between threads.
    # CTA 之间隔离 shared，线程之间隔离 local。
    def test_spaces_and_generic_translation(self):
        p = thread_addresses().emit('STL', ra=Z, rb=0, mode=4)
        p.emit('CVTA', rd=3, ra=Z, mode=1).emit('LD', rd=4, ra=3, mode=4)
        p.emit('CVTA', rd=5, ra=3, mode=3).emit('S2R', rd=6, imm=3)
        p.emit('MOVI', rd=7, imm=100).emit('IADD', rd=6, ra=6, rb=7)
        p.emit('CVTA', rd=8, ra=Z, mode=0).emit('ST', ra=8, rb=6, mode=4, guard=0)
        p.emit('BAR_SYNC', rd=0).emit('LDS', rd=9, ra=Z, mode=4).emit('EXIT')
        result = self.execute(p, '--blocks', 5, '--threads', 37)
        for block in range(5):
            for wi, count in [(0,32), (1,5)]:
                self.assertEqual(self.registers(result, 4, block, wi)[:count], list(range(wi*32, wi*32+count)))
                self.assertEqual(self.registers(result, 9, block, wi)[:count], [100+block]*count)
            self.assertEqual(word(result['ctas'][block]['shared'], 0), 100+block)
            self.assertEqual(word(result['ctas'][block]['local'][36], 0), 36)

    # Sparse addresses near the 32-bit limit never require a huge allocation.
    # 接近 32 位上限的稀疏地址无需巨额内存分配。
    def test_sparse_zero_normalization_and_high_address(self):
        p = Program().emit('MOVI', rd=0, imm=0xFFFFFFFC).emit('MOVI', rd=1, imm=0xAABBCCDD)
        p.emit('STG', ra=0, rb=1, mode=4).emit('LDG', rd=2, ra=0, mode=4)
        p.emit('STG', ra=0, rb=Z, mode=4).emit('LDG', rd=3, ra=Z, imm=4096, mode=4)
        result = self.execute(p.emit('EXIT'))
        self.assertEqual(self.registers(result, 2), [0xAABBCCDD]*32)
        self.assertEqual(self.registers(result, 3), [0]*32)
        self.assertEqual(result['global'], [])

    # Cross-warp reduction must agree under different PC and warp selection orders.
    # 跨 warp 规约在不同 PC 与 warp 选择顺序下必须一致。
    def test_multiwarp_shared_reduction(self):
        snapshots = []
        for policy in POLICIES:
            result = self.execute(reduction(37), '--blocks', 5, '--threads', 37,
                                  '--policy', policy, '--warp-policy', policy)
            self.assertEqual([word(result['global'], 4*b) for b in range(5)], [sum(range(37))]*5)
            snapshots.append(result['global'])
        self.assertEqual(snapshots[0], snapshots[1])
        self.assertEqual(snapshots[1], snapshots[2])

    # Returning atomics assign unique tickets across both SMs and all blocks.
    # 返回旧值的原子操作在两个 SM 和全部线程块之间分配唯一票号。
    def test_global_atomic_tickets_and_shared_reduction(self):
        p = Program().emit('MOVI', rd=0, imm=1)
        p.emit('ATOM', rd=1, ra=Z, rb=0).emit('RED', ra=Z, rb=0, aux=1)
        p.emit('BAR_SYNC', rd=0).emit('LDS', rd=2, ra=Z, mode=4).emit('EXIT')
        for policy in POLICIES:
            result = self.execute(p, '--blocks', 5, '--threads', 37, '--warp-policy', policy)
            tickets = []
            for b in range(5):
                tickets += self.registers(result, 1, b, 0) + self.registers(result, 1, b, 1)[:5]
                self.assertEqual(self.registers(result, 2, b, 0), [37]*32)
            self.assertEqual(sorted(tickets), list(range(185)))
            self.assertEqual(word(result['global'], 0), 185)

    # CAS returns old values and replaces only successful comparisons.
    # CAS 返回旧值，并只在比较成功时替换。
    def test_atomic_compare_exchange_and_modes(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=1)
        p.emit('IADD', rd=0, ra=0, rb=1).emit('ATOM', rd=2, ra=Z, rb=Z, rc=0, mode=2)
        result = self.execute(p.emit('EXIT'))
        self.assertEqual(self.registers(result, 2), [0]+[1]*31)
        self.assertEqual(word(result['global'], 0), 1)
        expected = [3, 4, 0xFFFFFFFF, 4, 0xFFFFFFFF, 0xFFFFFFFB, 4, 0xFFFFFFFF, 0xFFFFFFFF, 4]
        for mode, value in enumerate(expected):
            p = Program().emit('MOVI', rd=0, imm=4).emit('ATOM', rd=1, ra=Z, rb=0, mode=mode)
            result = self.execute(p.emit('EXIT'), '--threads', 1, images={'global': struct.pack('<I', 0xFFFFFFFF)})
            self.assertEqual(word(result['global'], 0), value)
            self.assertEqual(self.registers(result, 1)[0], 0xFFFFFFFF)

    # Divergent lanes arrive at different PCs without being counted as whole warps.
    # 分歧线程从不同 PC 到达，不得将它们按整个 warp 计数。
    def test_divergent_barrier_and_generation_reuse(self):
        p = thread_addresses().emit('MOVI', rd=1, imm=16)
        p.emit('ISETP', rd=1, ra=0, rb=1, mode=2).emit('STS', ra=2, rb=0, mode=4)
        p.emit('BRA', imm='other', guard=1).emit('BAR_SYNC', rd=3).emit('BRA', imm='end')
        p.label('other').emit('YIELD').emit('BAR_SYNC', rd=3)
        p.label('end').emit('LDS', rd=3, ra=Z, imm=124, mode=4)
        p.emit('BAR_SYNC', rd=3).emit('MOVI', rd=4, imm=99).emit('STS', ra=2, rb=4, mode=4)
        p.emit('BAR_SYNC', rd=3).emit('LDS', rd=5, ra=Z, mode=4).emit('EXIT')
        for policy in POLICIES:
            result = self.execute(p, '--policy', policy)
            self.assertEqual(self.registers(result, 3), [31]*32)
            self.assertEqual(self.registers(result, 5), [99]*32)

    # Dynamic barriers exclude early exits; explicit counts remain a strict contract.
    # 动态屏障排除提前退出线程；显式计数保持严格约束。
    def test_early_exit_and_explicit_count(self):
        p = thread_addresses().emit('EXIT', guard=0).emit('BAR_SYNC', rd=0).emit('EXIT')
        self.execute(p, '--threads', 37)
        p = thread_addresses().emit('EXIT', guard=0).emit('BAR_SYNC', rd=0, imm=37).emit('EXIT')
        report = self.execute(p, '--threads', 37, error='deadlock_no_runnable_group')
        self.assertIn('wait=cta_barrier token=0', report['error'])
        self.assertIn('block=0 sm=0 warp=1', report['error'])

    # Producers can arrive without blocking while consumers wait for all arrivals.
    # 生产者可仅到达而不阻塞，消费者等待全部到达。
    def test_arrival_only_and_misuse(self):
        p = Program().emit('S2R', rd=0, imm=2).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('BRA', imm='producer', guard=0).emit('BAR_SYNC', rd=0, imm=64).emit('EXIT')
        p.label('producer').emit('BAR_ARRIVE', rd=0, imm=64).emit('EXIT')
        self.execute(p, '--threads', 64)
        p = Program().emit('BAR_ARRIVE', rd=0, imm=64).emit('BAR_ARRIVE', rd=0, imm=64).emit('EXIT')
        self.execute(p, '--threads', 64, '--warp-policy', 'min', error='duplicate_barrier_arrival')
        self.execute(Program().emit('BAR_SYNC', rd=16), error='cta_barrier_id_out_of_range')
        self.execute(Program().emit('BAR_SYNC', imm=33), error='invalid_barrier_count')

    # Round-robin warp scheduling lets a producer progress while another warp spins.
    # warp 轮转调度让生产者在另一 warp 自旋时仍能前进。
    def test_fair_producer_consumer_progress(self):
        p = Program().emit('S2R', rd=0, imm=2).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('BRA', imm='producer', guard=8)
        p.label('spin').emit('LDG', rd=1, ra=Z, mode=4)
        p.emit('ISETP', rd=1, ra=1, rb=Z).emit('BRA', imm='spin', guard=1).emit('EXIT')
        p.label('producer')
        for _ in range(20): p.emit('NOP')
        p.emit('MOVI', rd=2, imm=1).emit('FENCE').emit('STG', ra=Z, rb=2, mode=4).emit('EXIT')
        result = self.execute(p, '--threads', 64, '--max-steps', 500)
        self.assertEqual(self.registers(result, 1), [1]*32)

    # Bounds, alignment, and address translation faults must remain explicit.
    # 越界、对齐和地址转换错误必须明确报告。
    def test_memory_faults_and_predicated_off_access(self):
        self.execute(Program().emit('LDS', rd=0, ra=Z, imm=65536), error='memory_out_of_bounds')
        self.execute(Program().emit('LDG', rd=0, ra=Z, imm=1, mode=4), error='misaligned_memory_access')
        self.execute(Program().emit('LDG', rd=0, ra=Z, imm=-1), error='memory_address_overflow')
        self.execute(Program().emit('CVTA', rd=0, ra=Z, mode=2), error='cvta_address_out_of_range')
        p = Program().emit('LDS', rd=0, ra=Z, imm=65536, guard=NOT_PT).emit('EXIT')
        self.assertEqual(self.execute(p)['ctas'][0]['shared'], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

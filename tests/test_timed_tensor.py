# Exercise matrix arithmetic through actual memory requests and a clocked multiply-accumulate array.
# 通过实际内存请求和时钟驱动乘加阵列检验矩阵运算。
import itertools
import math
import unittest
from support import SimulationCase, word
from tensor_support import launch_matrix, tensor_program
from test_tensor import TensorTest as FunctionalTensorTests
from test_tensor import dense_reference
from simt_lab import Program, Z, f32
from simt_lab.tensor import Image, MatrixDescriptor, pack_values, store_words
from simt_lab.workloads import gemm
from simt_lab.runtime import compare


def dense_input(m=7, n=6, k=9, format=1):
    image = Image()
    a = pack_values([(i % 9) - 4 for i in range(m*k)], format)
    b = pack_values([(i % 7) - 3 for i in range(k*n)], format)
    d = MatrixDescriptor(format, m, n, k, image.append(a), image.append(b), stride_a=k, stride_b=n)
    return image, d, a, b


# Preserve the exact independent rational references from functional validation.
# 保留功能验证中独立、精确的有理数参考。
class TimedTensorSemantics(FunctionalTensorTests):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', *options, **kwargs)


class TensorPipelineTest(SimulationCase):
    tile = FunctionalTensorTests.tile

    def timed(self, program, *options, **kwargs):
        return self.execute(program, '--hardware-trace', *options, **kwargs)

    # An unchanged workload traverses global input, MMA, TMEM loads, and global output on both SMs.
    # 不修改原工作负载，让两个 SM 完整执行 global 输入、MMA、TMEM 读取和 global 输出。
    def test_tiled_gemm_end_to_end(self):
        workload = gemm()
        result = self.timed(workload.program, '--blocks', workload.blocks, '--threads', workload.threads,
                            '--shared-bytes', 256, images={'global': bytes(workload.image.data)})
        self.assertEqual(compare(workload, result)['verified_values'], 96)
        self.assertEqual({cta['sm_id'] for cta in result['ctas']}, {0, 1})
        self.assertEqual(sum(s['products'] for s in result['hardware']['tensor']), 12*8*16)

    def test_maximum_descriptor_shape(self):
        image = Image()
        a, b = image.append(pack_values([1]*8192, 3)), image.append(pack_values([1]*8192, 3))
        d = MatrixDescriptor(3, 32, 32, 256, a, b)
        result = self.timed(launch_matrix(d), '--threads', 1, images={'global': bytes(image.data)})
        self.assertEqual(self.tile(result, d), [256]*1024)
        self.assertEqual(result['hardware']['tensor'][0]['products'], 262144)

    def test_microtiles_and_array_geometry(self):
        image, d, a, b = dense_input()
        reports = [self.timed(launch_matrix(d), '--tensor-rows', size, '--tensor-columns', size,
                             images={'global': bytes(image.data)}) for size in (2, 4)]
        for size, result in zip((2, 4), reports):
            self.assertEqual(self.tile(result, d), dense_reference(d, a, b))
            tensor = result['hardware']['tensor'][0]
            self.assertEqual(tensor['tiles'], math.ceil(d.m/size) * math.ceil(d.n/size))
            self.assertEqual(tensor['products'], d.m*d.n*d.k)
            self.assertGreater(result['hardware']['dram']['data_reads'], 0)
        self.assertGreater(reports[0]['hardware']['cycles'], reports[1]['hardware']['cycles'])

    # Real shared stores populate the descriptor and input tiles before the tensor frontend reads.
    # 真实 shared 写入先填充描述符和输入 tile，张量前端随后读取。
    def test_shared_operand_supply_and_array_latency(self):
        d = MatrixDescriptor(0, 4, 4, 8, 256, 512, space=0)
        p = tensor_program(d, columns=8)
        store_words(p, 256, [f32(2)]*32)
        store_words(p, 512, [f32(3)]*32)
        p.emit('MMA', ra=Z, guard=0).emit('EXIT')
        fast = self.timed(p, '--threads', 1, '--tensor-latency', 1)
        slow = self.timed(p, '--threads', 1, '--tensor-latency', 32, '--tensor-queue', 1)
        for result in (fast, slow):
            self.assertEqual(self.tile(result, d), [f32(48)]*16)
            self.assertEqual(result['hardware']['dram']['data_reads'], 0)
            self.assertEqual(result['hardware']['tensor'][0]['products'], 128)
        self.assertGreater(slow['hardware']['cycles'], fast['hardware']['cycles'])
        self.assertGreater(slow['hardware']['tensor'][0]['queue_stalls'], 0)

    # Different sparse rows select different B coordinates; missing products must not propagate NaN.
    # 不同稀疏行选择不同 B 坐标，缺失乘积不得传播 NaN。
    def test_sparse_rows_and_absent_nan(self):
        image = Image()
        pairs = list(itertools.combinations(range(4), 2))
        a = image.append(pack_values([2, 3]*6, 0))
        b = image.append(pack_values([1, 10, 100, 1000], 0))
        metadata = image.append(bytes(first | second << 2 for first, second in pairs))
        d = MatrixDescriptor(0, 6, 1, 4, a, b, flags=2, metadata=metadata, stride_metadata=1)
        result = self.timed(launch_matrix(d), images={'global': bytes(image.data)})
        self.assertEqual(self.tile(result, d), [f32(2*10**first + 3*10**second) for first, second in pairs])
        self.assertEqual(result['hardware']['tensor'][0]['products'], 12)
        self.assertEqual(result['hardware']['tensor'][0]['skipped'], 12)
        image.data[b+8:b+16] = pack_values([float('nan'), float('nan')], 0)
        d.m = 1
        result = self.timed(launch_matrix(d), images={'global': bytes(image.data)})
        self.assertEqual(self.tile(result, d), [f32(32)])

    def test_tmem_bank_conflicts_and_broadcast(self):
        reports = []
        for row, column in ((0, Z), (Z, 0), (Z, Z)):
            p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z)
            p.emit('TMEM_ALLOC', rd=1, imm=32, guard=0)
            p.emit('LDT', rd=2, ra=row, rb=column).emit('EXIT')
            reports.append(self.timed(p))
        self.assertEqual(reports[1]['hardware']['cycles'] - reports[0]['hardware']['cycles'], 30)
        self.assertEqual(reports[2]['hardware']['tensor'][0]['tmem_broadcasts'], 31)
        self.assertEqual(self.registers(reports[1], 2), [0]*32)

    def test_allocation_zeroing_uses_ports(self):
        p = Program().emit('TMEM_ALLOC', rd=0, imm=8).emit('EXIT')
        reports = [self.timed(p, '--threads', 1, '--tmem-banks', banks) for banks in (8, 32)]
        self.assertGreater(reports[0]['hardware']['cycles'], reports[1]['hardware']['cycles'])
        self.assertEqual(reports[0]['hardware']['tensor'][0]['tmem_bank_services'], 1024)
        self.timed(p, '--threads', 1, '--blocks', 6, '--sms', 1, '--tmem-columns', 8, '--max-ctas', 1)

    def test_tmem_capacity_is_shared_between_ctas(self):
        p = Program().emit('TMEM_ALLOC', rd=0, imm=8)
        p.emit('MOVI', rd=1, imm=100).emit('MOVI', rd=2, imm=1)
        p.label('hold').emit('ISUB', rd=1, ra=1, rb=2).emit('ISETP', rd=0, ra=1, rb=Z)
        p.emit('BRA', imm='hold', guard=8).emit('EXIT')
        self.timed(p, '--threads', 1, '--blocks', 2, '--sms', 1, '--shared-bytes', 256,
                   '--tmem-columns', 8, error='tensor_out_of_columns')

    def test_other_warp_progresses_during_mma(self):
        image, d, _, _ = dense_input(4, 4, 16)
        p = tensor_program(d, columns=8).emit('S2R', rd=2, imm=2).emit('ISETP', rd=1, ra=2, rb=Z)
        p.emit('BRA', imm='worker', guard=9)
        mma_pc = len(p.instructions)
        p.emit('MMA', ra=Z, guard=0).emit('BRA', imm='done').label('worker')
        for _ in range(40):
            p.emit('IADD', rd=8, ra=8, rb=Z)
        p.label('done').emit('EXIT')
        result = self.timed(p, '--threads', 64, images={'global': bytes(image.data)})
        trace = result['hardware']['trace']
        start = next(e['cycle'] for e in trace if e['pc'] == mma_pc and e['action'] == 'execute')
        end = next(e['cycle'] for e in trace if e['pc'] == mma_pc and e['action'] == 'retire')
        self.assertTrue(any(e['warp'] == 1 and e['action'] == 'retire' and start < e['cycle'] < end for e in trace))
        stores = [e['cycle'] for e in trace if e['action'] == 'tensor_tile_store']
        self.assertLess(max(stores), end)


del FunctionalTensorTests

if __name__ == '__main__':
    unittest.main(verbosity=2)

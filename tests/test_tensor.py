# Compare descriptor-driven MMA against exact rational and integer references.
# 将描述符驱动的 MMA 与精确有理数及整数参考比较。
from fractions import Fraction
import itertools
import random
import struct
import unittest
from support import SimulationCase, word
from numeric_reference import binary_value, element, fused, rounded
from tensor_support import launch_matrix, tensor_program
from simt_lab import Program, Z, f32
from simt_lab.tensor import Image, MatrixDescriptor, pack_values, store_words


# Construct pitched matrices so accidental dense indexing cannot pass.
# 构造带行间距的矩阵，防止错误的紧密索引也通过测试。
def dense_case(format, seed):
    rng = random.Random(seed)
    image = Image()
    values = [rng.uniform(-6, 6) if format != 3 else rng.randint(-128, 127) for _ in range(3*9+7*7)]
    a_data, b_data = pack_values(values[:27], format), pack_values(values[27:], format)
    a, b = image.append(a_data), image.append(b_data)
    return image, MatrixDescriptor(format, 3, 5, 7, a, b, stride_a=9, stride_b=7), a_data, b_data


# Compute the reference from decoded input bytes, never from unquantized input values.
# 从解码后的输入字节计算参考，不使用量化前数值。
def dense_reference(d, a, b, initial=0):
    output = []
    for row, column in itertools.product(range(d.m), range(d.n)):
        accumulator = initial
        for k in range(d.k):
            x, y = element(a, d.format, row*d.stride_a+k), element(b, d.format, k*d.stride_b+column)
            accumulator = (accumulator + x*y) & 0xFFFFFFFF if d.format == 3 else fused(x, y, accumulator)
        output.append(accumulator)
    return output


class TensorTest(SimulationCase):
    # Read the public column-major TMEM layout independently of tensor instructions.
    # 独立于张量指令，读取公开的 TMEM 列优先布局。
    def tile(self, report, d):
        snapshot = report['ctas'][0]['tmem']
        return [word(snapshot, ((d.column_d+c)*128+d.row_d+r)*4)
                for r in range(d.m) for c in range(d.n)]

    # Validate the rational oracle with known halfway, subnormal, and overflow cases.
    # 用已知中点、次正规数和溢出案例校验有理数参考。
    def test_reference_rounding_boundaries(self):
        self.assertEqual(rounded(Fraction(1) + Fraction(1, 1 << 24)), 0x3F800000)
        self.assertEqual(rounded(Fraction(1) + Fraction(3, 1 << 24)), 0x3F800002)
        self.assertEqual(rounded(Fraction(1, 1 << 149)), 1)
        self.assertEqual(rounded(Fraction(1, 1 << 150)), 0)
        self.assertEqual(rounded(Fraction(3, 1 << 150)), 2)
        self.assertEqual(rounded(Fraction(1 << 128)), 0x7F800000)

    # All five formats exercise nontrivial strides and independent exact accumulation.
    # 五种格式都覆盖非紧密步长与独立精确累加参考。
    def test_dense_formats_against_exact_reference(self):
        for format, seed in itertools.product(range(5), (17, 36, 105)):
            image, d, a, b = dense_case(format, seed)
            with self.subTest(format=format, seed=seed):
                result = self.execute(launch_matrix(d), images={'global': bytes(image.data)})
                self.assertEqual(self.tile(result, d), dense_reference(d, a, b))

    # Subnormal inputs must survive conversion instead of being silently flushed.
    # 次正规输入必须在转换后保留，不能静默清零。
    def test_half_bfloat_and_float_subnormals(self):
        for format, bits, size in [(0,1,4), (1,1,2), (2,1,2)]:
            image = Image()
            a_data = bits.to_bytes(size, 'little')
            b_data = pack_values([1], format)
            d = MatrixDescriptor(format, 1, 1, 1, image.append(a_data), image.append(b_data))
            result = self.execute(launch_matrix(d), images={'global': bytes(image.data)})
            self.assertEqual(self.tile(result, d), [rounded(element(a_data, format, 0))])

    # Accumulation must use a fused product and preserve integer wraparound.
    # 累加必须使用融合乘积，并保留整数绕回语义。
    def test_accumulator_fused_rounding_and_integer_wrap(self):
        image = Image()
        a, b = image.append(pack_values([1+2**-23], 0)), image.append(pack_values([1-2**-23], 0))
        d = MatrixDescriptor(0, 1, 1, 1, a, b, flags=1)
        result = self.execute(launch_matrix(d, previous=f32(-1)), images={'global': bytes(image.data)})
        self.assertEqual(self.tile(result, d), [f32(-2**-46)])
        image = Image()
        d = MatrixDescriptor(3, 1, 1, 1, image.append(pack_values([127], 3)), image.append(pack_values([127], 3)), flags=1)
        result = self.execute(launch_matrix(d, previous=0xFFFFFFF0), images={'global': bytes(image.data)})
        self.assertEqual(self.tile(result, d), [(0xFFFFFFF0+127*127) & 0xFFFFFFFF])

    # Every legal 2:4 position pair must select the matching logical B elements.
    # 每种合法 2:4 位置组合都必须选择对应的逻辑 B 元素。
    def test_sparse_metadata_positions_and_rejection(self):
        for first, second in itertools.combinations(range(4), 2):
            image = Image()
            a, b = image.append(pack_values([2,3], 1)), image.append(pack_values([1,10,100,1000], 1))
            metadata = image.append(bytes([first | (second << 2)]))
            d = MatrixDescriptor(1, 1, 1, 4, a, b, flags=2, metadata=metadata, stride_metadata=1)
            result = self.execute(launch_matrix(d), images={'global': bytes(image.data)})
            self.assertEqual(self.tile(result, d), [f32(2*10**first + 3*10**second)])
        for invalid in (0, 0xF4, 3):
            image.data[metadata] = invalid
            self.execute(launch_matrix(d), images={'global': bytes(image.data)}, error='invalid_sparse_metadata')

    # Distinct row/block and block/column scales detect swapped scale indexing.
    # 不同的行/块及块/列缩放系数用于检测缩放索引颠倒。
    def test_fp4_block_scales(self):
        image = Image()
        a_data = pack_values([1,.5,2,3,4,6,1,2, 6,4,3,2,1,.5,2,4], 4)
        b_data = pack_values([1,2,3,4,6,1,2,3]*3, 4)
        a, b = image.append(a_data), image.append(b_data)
        sa_values, sb_values = [.5,2,3,.25], [1,2,4, .5,1.5,3]
        sa, sb = image.append(pack_values(sa_values, 0)), image.append(pack_values(sb_values, 0))
        d = MatrixDescriptor(4, 2, 3, 8, a, b, flags=4, block_k=4,
                             scale_a=sa, scale_b=sb, stride_scale_a=2, stride_scale_b=3)
        result = self.execute(launch_matrix(d), images={'global': bytes(image.data)})
        expected = []
        for r, c in itertools.product(range(2), range(3)):
            accumulator = 0
            for block in range(2):
                dot = 0
                for k in range(block*4, block*4+4):
                    dot = fused(element(a_data,4,r*8+k), element(b_data,4,k*3+c), dot)
                scale = Fraction(sa_values[r*2+block]) * Fraction(sb_values[block*3+c])
                accumulator = fused(binary_value(dot,23,8,127), scale, accumulator)
            expected.append(accumulator)
        self.assertEqual(self.tile(result, d), expected)

    # Multiple MMA instructions can retain D in TMEM across K tiles.
    # 多条 MMA 指令可在 K tile 之间将 D 保留在 TMEM。
    def test_repeated_mma_accumulates_in_tmem(self):
        image = Image()
        a, b = image.append(pack_values([2,3], 0)), image.append(pack_values([5,7], 0))
        d = MatrixDescriptor(0,1,1,2,a,b,flags=1)
        p = tensor_program(d).emit('MMA',ra=Z,guard=0).emit('MMA',ra=Z,guard=0).emit('EXIT')
        result = self.execute(p, images={'global': bytes(image.data)})
        self.assertEqual(self.tile(result,d), [f32(62)])

    # Combine sparse K reconstruction with nontrivial scale-product rounding.
    # 组合稀疏 K 还原与非平凡缩放系数乘积舍入。
    def test_sparse_scaled_rounding_and_invalid_scale(self):
        image = Image()
        a, b = image.append(pack_values([2,3,4,5],1)), image.append(pack_values(range(1,9),1))
        metadata = image.append(bytes([12,9]))
        sa = image.append(pack_values([.1,.3],0))
        sb = image.append(pack_values([.2,.7],0))
        d = MatrixDescriptor(1,1,1,8,a,b,flags=6,metadata=metadata,stride_metadata=2,
                             scale_a=sa,scale_b=sb,block_k=4,stride_scale_a=2,stride_scale_b=1)
        expected = 0
        for block, dot in enumerate([2*1+3*4, 4*6+5*7]):
            a_scale = element(image.data[sa:],0,block)
            b_scale = element(image.data[sb:],0,block)
            product = binary_value(rounded(a_scale*b_scale),23,8,127)
            expected = fused(Fraction(dot),product,expected)
        result = self.execute(launch_matrix(d),images={'global':bytes(image.data)})
        self.assertEqual(self.tile(result,d),[expected])
        image.data[sa:sa+4] = struct.pack('<f',-1)
        self.execute(launch_matrix(d),images={'global':bytes(image.data)},error='invalid_block_scale')

    # Freed columns are inaccessible and reallocation clears their previous contents.
    # 已释放列不可访问，再次分配会清除旧内容。
    def test_tmem_allocation_transfer_and_reuse(self):
        p = Program().emit('S2R',rd=0,imm=0).emit('ISETP',rd=0,ra=0,rb=Z)
        p.emit('TMEM_ALLOC',rd=1,imm=8,guard=0).emit('TMEM_ALLOC',rd=2,imm=8,guard=0)
        p.emit('MOVI',rd=3,imm=8).emit('STT',ra=0,rb=3,rc=0).emit('LDT',rd=4,ra=0,rb=3)
        p.emit('TMEM_FREE',ra=2,imm=8,guard=0).emit('TMEM_ALLOC',rd=5,imm=8,guard=0)
        p.emit('LDT',rd=6,ra=0,rb=3).emit('EXIT')
        result = self.execute(p)
        self.assertEqual(self.registers(result,4),list(range(32)))
        self.assertEqual(self.registers(result,6),[0]*32)
        self.assertEqual(self.registers(result,5)[0],8)
        self.execute(Program().emit('LDT',rd=0,ra=Z,rb=Z),error='tensor_column_not_allocated')

    # Reject invalid pitches, shapes, allocation ownership, and issue participation.
    # 拒绝无效步长、形状、分配归属和命令参与集合。
    def test_invalid_tensor_descriptors(self):
        image, d, a, b = dense_case(0, 17)
        for name, value, error in [('m',33,'invalid_matrix_shape'), ('stride_a',1,'invalid_matrix_pitch'),
                                   ('column_d',31,'tensor_column_not_allocated'), ('flags',8,'invalid_matrix_flags')]:
            old = getattr(d,name)
            setattr(d,name,value)
            self.execute(launch_matrix(d),images={'global':bytes(image.data)},error=error)
            setattr(d,name,old)
        self.execute(Program().emit('TMEM_ALLOC',rd=0,imm=8),error='tensor_single_issuer_required')
        self.execute(Program().emit('TMEM_ALLOC',rd=0,imm=7),'--threads',1,error='invalid_tensor_allocation')
        self.execute(Program().emit('TMEM_FREE',ra=Z,imm=8),'--threads',1,error='invalid_tensor_free')


if __name__ == '__main__':
    unittest.main(verbosity=2)

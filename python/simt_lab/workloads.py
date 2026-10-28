# Each workload supplies data, real machine code, and a separate mathematical expectation.
# 每个工作负载提供数据、真实机器码和独立数学期望。
from dataclasses import dataclass
import math
import struct
from .program import Z
from .tensor import Image, MatrixDescriptor, pack_values, store_words
from .kernels import start_kernel, index_tile


@dataclass
class Workload:
    name: str
    program: object
    image: Image
    output: int
    expected: list
    blocks: int = 1
    threads: int = 32
    kind: str = 'float'
    atol: float = 1e-5
    rtol: float = 1e-5


# Global indexing includes the last partially occupied vector tile.
# 全局索引包含最后一个未占满的向量 tile。
def vector_add(length=257):
    image = Image()
    a = [(i%17-8)/4 for i in range(length)]
    b = [(i%11-5)/8 for i in range(length)]
    base_a, base_b = image.append(pack_values(a,0)), image.append(pack_values(b,0))
    output = image.append(bytes(4*length))
    p = start_kernel().emit('S2R',rd=2,imm=3).emit('MOVI',rd=3,imm=64)
    p.emit('IMAD',rd=4,ra=2,rb=3,rc=0).emit('MOVI',rd=5,imm=length)
    p.emit('ISETP',rd=1,ra=4,rb=5,mode=2).emit('MOVI',rd=5,imm=4)
    p.emit('IMUL',rd=4,ra=4,rb=5)
    p.emit('LDG',rd=6,ra=4,imm=base_a,mode=4,guard=1)
    p.emit('LDG',rd=7,ra=4,imm=base_b,mode=4,guard=1).emit('FADD',rd=8,ra=6,rb=7)
    p.emit('STG',ra=4,rb=8,imm=output,mode=4,guard=1).emit('EXIT')
    return Workload('vector',p,image,output,[x+y for x,y in zip(a,b)],math.ceil(length/64),64)


# Each warp first produces one integer partial sum through explicit shuffles.
# 每个 warp 先通过显式 SHFL 生成一个整数部分和。
def warp_integer_sum(p):
    for shift in range(5):
        p.emit('MOVI',rd=8,imm=1 << shift).emit('XOR',rd=9,ra=1,rb=8)
        p.emit('SHFL',rd=10,ra=7,rb=9,imm=0xFFFFFFFF).emit('IADD',rd=7,ra=7,rb=10)
    p.emit('S2R',rd=8,imm=2).emit('MOVI',rd=9,imm=4).emit('IMUL',rd=8,ra=8,rb=9)
    p.emit('ISETP',rd=2,ra=1,rb=Z).emit('STS',ra=8,rb=7,mode=4,guard=2)
    p.emit('BAR_SYNC',rd=0)


# Cross-warp publication uses shared memory and a real CTA synchronization point.
# 跨 warp 发布使用 shared 和真实 CTA 同步点。
def reduction(count=137, blocks=3):
    image = Image()
    values = [(i*7+3)%101 for i in range(count*blocks)]
    image.append(struct.pack('<'+'I'*len(values),*values))
    output = image.append(bytes(4*blocks))
    p = start_kernel().emit('S2R',rd=2,imm=3).emit('MOVI',rd=3,imm=count)
    p.emit('IMAD',rd=4,ra=2,rb=3,rc=0).emit('ISETP',rd=1,ra=0,rb=3,mode=2)
    p.emit('MOVI',rd=5,imm=4).emit('IMUL',rd=4,ra=4,rb=5)
    p.emit('LDG',rd=7,ra=4,mode=4,guard=1)
    warp_integer_sum(p)
    for warp in range(math.ceil(count/32)):
        p.emit('LDS',rd=11,ra=Z,imm=warp*4,mode=4,guard=0)
        p.emit('IADD',rd=12,ra=12,rb=11,guard=0)
    p.emit('IMUL',rd=13,ra=2,rb=5).emit('STG',ra=13,rb=12,imm=output,mode=4,guard=0).emit('EXIT')
    expected = [sum(values[b*count:(b+1)*count]) for b in range(blocks)]
    return Workload('reduction',p,image,output,expected,blocks,math.ceil(count/32)*32,'uint32')


# Produce deterministic FP16 inputs and reference the values after quantization.
# 生成确定性 FP16 输入，并对量化后的数值求参考。
def gemm_inputs():
    image = Image()
    a_data = pack_values([((i*3)%19-9)/7 for i in range(12*16)],1)
    b_data = pack_values([((i*5)%17-8)/9 for i in range(16*8)],1)
    a, b = image.append(a_data), image.append(b_data)
    output = image.append(bytes(12*8*4))
    a_values = struct.unpack('<'+'e'*(12*16),a_data)
    b_values = struct.unpack('<'+'e'*(16*8),b_data)
    expected = [sum(a_values[r*16+k]*b_values[k*8+c] for k in range(16)) for r in range(12) for c in range(8)]
    return image,a,b,output,expected


# Three CTAs compute different row tiles on the two-SM device.
# 三个 CTA 在双 SM 设备上计算不同的行 tile。
def gemm():
    image,a,b,output,expected = gemm_inputs()
    p = start_kernel().emit('TMEM_ALLOC',rd=2,imm=8,guard=0)
    store_words(p,0,MatrixDescriptor(1,4,8,16,a,b).words())
    p.emit('S2R',rd=4,imm=3).emit('MOVI',rd=5,imm=128)
    p.emit('IMUL',rd=6,ra=4,rb=5).emit('MOVI',rd=7,imm=a).emit('IADD',rd=7,ra=7,rb=6)
    p.emit('STS',ra=Z,rb=7,imm=24,mode=4,guard=0)
    p.emit('BAR_SYNC',rd=0).emit('MMA',ra=Z,guard=0).emit('BAR_SYNC',rd=0)
    index_tile(p,4,8,output)
    p.emit('LDT',rd=20,ra=9,rb=10).emit('IADD',rd=12,ra=12,rb=6)
    p.emit('STG',ra=12,rb=20,mode=4).emit('BAR_SYNC',rd=0)
    p.emit('TMEM_FREE',ra=2,imm=8,guard=0).emit('EXIT')
    return Workload('gemm',p,image,output,expected,3,32)

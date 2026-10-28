# Compose short instruction sequences with explicit scratch registers and synchronization.
# 使用明确的临时寄存器与同步点组合短指令序列。
import math
from .program import Program, Z, f32
from .tensor import MatrixDescriptor, store_words

FULL = 0xFFFFFFFF


# Reserve R0 for CTA thread ID, R1 for lane ID, and P0 for the CTA leader.
# 保留 R0 作为 CTA 线程 ID、R1 作为 lane ID、P0 作为 CTA leader 谓词。
def start_kernel():
    p = Program().emit('S2R',rd=0,imm=1).emit('S2R',rd=1,imm=0)
    return p.emit('ISETP',rd=0,ra=0,rb=Z)


# R9/R10 identify the row/column; R12 addresses one row-major shared element.
# R9/R10 标识行列，R12 定位一个行优先 shared 元素。
def index_tile(p, rows, columns, address):
    if columns <= 0 or columns & (columns-1):
        raise ValueError('tile indexing requires power-of-two columns')
    p.emit('MOVI',rd=8,imm=columns.bit_length()-1).emit('SHR',rd=9,ra=0,rb=8)
    p.emit('MOVI',rd=10,imm=columns-1).emit('AND',rd=10,ra=0,rb=10)
    p.emit('MOVI',rd=11,imm=4).emit('MOVI',rd=13,imm=address)
    p.emit('IMAD',rd=12,ra=0,rb=11,rc=13).emit('MOVI',rd=14,imm=rows*columns)
    p.emit('ISETP',rd=1,ra=0,rb=14,mode=2)


# XOR exchanges reduce independent power-of-two groups within each warp.
# XOR 交换在每个 warp 内归约独立的二次幂大小线程组。
def reduce_groups(p, register, width, operation='sum'):
    for shift in range(width.bit_length()-1):
        p.emit('MOVI',rd=25,imm=1 << shift).emit('XOR',rd=26,ra=1,rb=25)
        p.emit('SHFL',rd=27,ra=register,rb=26,imm=FULL)
        if operation == 'sum':
            p.emit('FADD',rd=register,ra=register,rb=27)
        else:
            p.emit('FSETP',rd=2,ra=register,rb=27,mode=2)
            p.emit('MOV',rd=register,ra=27,guard=2)


# Normalize each row with unit affine scale and zero affine bias.
# 每行归一化，仿射缩放为一，仿射偏置为零。
def layer_norm(p, source, destination, rows=4, columns=8):
    index_tile(p,rows,columns,source)
    p.emit('MOVI',rd=20,imm=0).emit('LDS',rd=20,ra=12,mode=4,guard=1)
    p.emit('MOV',rd=21,ra=20)
    reduce_groups(p,21,columns)
    p.emit('MOVI',rd=22,imm=f32(1/columns)).emit('FMUL',rd=21,ra=21,rb=22)
    p.emit('MOVI',rd=23,imm=f32(-1)).emit('FFMA',rd=24,ra=21,rb=23,rc=20)
    p.emit('FMUL',rd=21,ra=24,rb=24)
    reduce_groups(p,21,columns)
    p.emit('FMUL',rd=21,ra=21,rb=22).emit('MOVI',rd=23,imm=f32(1e-5))
    p.emit('FADD',rd=21,ra=21,rb=23).emit('FRSQRT',rd=21,ra=21)
    p.emit('FMUL',rd=24,ra=24,rb=21)
    index_tile(p,rows,columns,destination)
    p.emit('STS',ra=12,rb=24,mode=4,guard=1).emit('BAR_SYNC',rd=0)


# Transfer a matrix result from TMEM into shared memory using one cell per thread.
# 每个线程搬运一个单元，将矩阵结果从 TMEM 写入 shared。
def drain_tile(p, destination, rows, columns):
    index_tile(p,rows,columns,destination)
    p.emit('LDT',rd=20,ra=9,rb=10,guard=1)
    p.emit('STS',ra=12,rb=20,mode=4,guard=1).emit('BAR_SYNC',rd=0)


# Reuse the allocated TMEM tile while keeping descriptor publication explicit.
# 复用已经分配的 TMEM tile，并显式发布描述符。
def shared_matrix(p, a, b, destination, m, n, k):
    descriptor = MatrixDescriptor(0,m,n,k,a,b,space=0)
    store_words(p,0,descriptor.words())
    p.emit('BAR_SYNC',rd=0).emit('MMA',ra=Z,guard=0).emit('BAR_SYNC',rd=0)
    drain_tile(p,destination,m,n)


# Transpose K by explicit lane address arithmetic before the QK matrix product.
# 在 QK 矩阵乘法之前，通过显式线程地址运算转置 K。
def transpose(p, source, destination, rows=4, columns=8):
    index_tile(p,rows,columns,source)
    p.emit('LDS',rd=20,ra=12,mode=4,guard=1).emit('MOVI',rd=21,imm=rows)
    p.emit('IMAD',rd=22,ra=10,rb=21,rc=9).emit('MOVI',rd=23,imm=destination)
    p.emit('IMAD',rd=22,ra=22,rb=11,rc=23)
    p.emit('STS',ra=22,rb=20,mode=4,guard=1).emit('BAR_SYNC',rd=0)


# Stabilize each softmax row by subtracting its maximum before exponentiation.
# 指数运算之前减去每行最大值，以稳定 softmax。
def softmax(p, source, destination, rows=4, columns=4, scale=1/math.sqrt(8)):
    index_tile(p,rows,columns,source)
    p.emit('MOVI',rd=20,imm=0).emit('LDS',rd=20,ra=12,mode=4,guard=1)
    p.emit('MOVI',rd=22,imm=f32(scale)).emit('FMUL',rd=20,ra=20,rb=22)
    p.emit('MOV',rd=21,ra=20)
    reduce_groups(p,21,columns,'max')
    p.emit('MOVI',rd=22,imm=f32(-1)).emit('FFMA',rd=20,ra=21,rb=22,rc=20)
    p.emit('MOVI',rd=22,imm=f32(math.log2(math.e))).emit('FMUL',rd=20,ra=20,rb=22)
    p.emit('FEXP2',rd=20,ra=20).emit('MOV',rd=21,ra=20)
    reduce_groups(p,21,columns)
    p.emit('FRCP',rd=21,ra=21).emit('FMUL',rd=20,ra=20,rb=21)
    index_tile(p,rows,columns,destination)
    p.emit('STS',ra=12,rb=20,mode=4,guard=1).emit('BAR_SYNC',rd=0)


# Add residual paths using ordinary lane-wise floating-point instructions.
# 使用普通逐线程浮点指令相加残差路径。
def residual(p, a, b, destination, rows=4, columns=8):
    index_tile(p,rows,columns,a)
    p.emit('LDS',rd=20,ra=12,mode=4,guard=1)
    index_tile(p,rows,columns,b)
    p.emit('LDS',rd=21,ra=12,mode=4,guard=1).emit('FADD',rd=20,ra=20,rb=21)
    index_tile(p,rows,columns,destination)
    p.emit('STS',ra=12,rb=20,mode=4,guard=1).emit('BAR_SYNC',rd=0)


# ReLU uses a predicate rather than a host-computed activation tensor.
# ReLU 使用谓词，不使用宿主预先计算的激活张量。
def relu(p, address, rows=4, columns=16):
    index_tile(p,rows,columns,address)
    p.emit('LDS',rd=20,ra=12,mode=4,guard=1).emit('FSETP',rd=2,ra=20,rb=Z,mode=2)
    p.emit('MOV',rd=20,ra=Z,guard=2).emit('STS',ra=12,rb=20,mode=4,guard=1)
    p.emit('BAR_SYNC',rd=0)


# Initialize one bulk transaction and wait before any shared operand is consumed.
# 初始化一个批量事务，并在使用任何 shared 操作数之前等待完成。
def stage_inputs(p, size, destination=256):
    p.emit('MOVI',rd=4,imm=128).emit('MOVI',rd=5,imm=destination)
    p.emit('MB_INIT',ra=4,imm=1,guard=0).emit('MB_EXPECT',ra=4,imm=size,guard=0)
    p.emit('MB_ARRIVE',rd=6,ra=4,guard=0).emit('CP_ASYNC',ra=Z,rb=5,rc=4,imm=size,guard=0)
    p.emit('BAR_SYNC',rd=0).emit('MB_WAIT',ra=4,rb=6)


# Copy the final shared output into the host-visible global image.
# 将最终 shared 输出复制到宿主可见的 global 镜像。
def publish(p, source, destination, rows=4, columns=8):
    index_tile(p,rows,columns,source)
    p.emit('LDS',rd=20,ra=12,mode=4,guard=1)
    index_tile(p,rows,columns,destination)
    p.emit('STG',ra=12,rb=20,mode=4,guard=1).emit('BAR_SYNC',rd=0)

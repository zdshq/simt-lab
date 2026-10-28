# Build public matrix descriptors and input images without executing device operations.
# 构造公开矩阵描述符与输入镜像，不代替设备执行运算。
from dataclasses import dataclass
import math
from pathlib import Path
import struct
from .program import f32, Z

FP32, FP16, BF16, INT8, FP4 = range(5)


# Round binary32 to bfloat16 using the retained low bit for ties-to-even.
# 根据保留部分的最低位，将 binary32 按最近偶数舍入为 bfloat16。
def bf16(value):
    bits = f32(value)
    if (bits & 0x7FFFFFFF) > 0x7F800000:
        return 0x7FC0
    return ((bits + 0x7FFF + ((bits >> 16) & 1)) >> 16) & 0xFFFF


# FP4 E2M1 has finite magnitudes only; out-of-range finite inputs saturate.
# FP4 E2M1 只包含有限幅度，越界的有限输入采用饱和处理。
def fp4(value):
    if not math.isfinite(value):
        raise ValueError('FP4 requires finite input')
    magnitudes = (0, .5, 1, 1.5, 2, 3, 4, 6)
    code = min(range(8), key=lambda index: (abs(abs(value)-magnitudes[index]), index % 2))
    return code | (8 if math.copysign(1, value) < 0 else 0)


# Pack row-major elements; FP4 puts the first element in the low nibble.
# 打包行优先元素；FP4 的第一个元素放在低半字节。
def pack_values(values, format):
    values = list(values)
    if format in (FP32, FP16, INT8):
        code = {FP32: 'f', FP16: 'e', INT8: 'b'}[format]
        return struct.pack('<' + code * len(values), *values)
    if format == BF16:
        return struct.pack('<' + 'H' * len(values), *(bf16(v) for v in values))
    if format != FP4:
        raise ValueError('unknown tensor format')
    codes = [fp4(value) for value in values]
    if len(codes) % 2:
        codes.append(0)
    return bytes(codes[index] | (codes[index+1] << 4) for index in range(0, len(codes), 2))


@dataclass
class MatrixDescriptor:
    format: int
    m: int
    n: int
    k: int
    a: int
    b: int
    flags: int = 0
    stride_a: int | None = None
    stride_b: int | None = None
    row_d: int = 0
    column_d: int = 0
    metadata: int = 0
    stride_metadata: int = 0
    scale_a: int = 0
    scale_b: int = 0
    block_k: int = 0
    stride_scale_a: int = 0
    stride_scale_b: int = 0
    space: int = 1

    # Default pitches describe dense rows unless sparse A is explicitly selected.
    # 默认步长表示紧密行排列；显式选择稀疏 A 时使用压缩行宽。
    def words(self):
        stride_a = self.stride_a if self.stride_a is not None else self.k // 2 if self.flags & 2 else self.k
        stride_b = self.stride_b if self.stride_b is not None else self.n
        return [0x31414D4D, self.format, self.flags, self.m, self.n, self.k, self.a, self.b,
                stride_a, stride_b, self.row_d, self.column_d, self.metadata, self.stride_metadata,
                self.scale_a, self.scale_b, self.block_k, self.stride_scale_a, self.stride_scale_b, self.space]


class Image:
    # Keep host data separate from encoded kernel instructions.
    # 将宿主数据与编码后的 kernel 指令分开。
    def __init__(self):
        self.data = bytearray()

    # Return each aligned payload's device byte address.
    # 返回每份对齐载荷的设备字节地址。
    def append(self, payload, alignment=16):
        if alignment <= 0:
            raise ValueError('positive alignment required')
        self.data.extend(bytes((-len(self.data)) % alignment))
        address = len(self.data)
        self.data.extend(payload)
        return address

    # Save an image accepted by the runner's global or constant loader.
    # 保存运行器 global 或 constant 装载接口接受的镜像。
    def save(self, path):
        Path(path).write_bytes(self.data)


# Emit ordinary stores so descriptors are created by actual simulated instructions.
# 发射普通存储指令，让描述符由实际模拟指令创建。
def store_words(program, address, words, guard=0, scratch=61):
    for index, value in enumerate(words):
        program.emit('MOVI', rd=scratch, imm=value, guard=guard)
        program.emit('STS', ra=Z, rb=scratch, imm=address+4*index, mode=4, guard=guard)
    return program

# Instruction arguments correspond directly to the public ISA fields.
# 指令参数直接对应公开的 ISA 字段。
import struct
from pathlib import Path
from simt_isa import encode

Z = 63
PT = 7
NOT_PT = 15


# Represent a binary32 literal as the machine's raw register bits.
# 将 binary32 常量表示为机器寄存器的原始位模式。
def f32(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


class Program:
    # Keep assembly statements separate from emitted machine code.
    # 将汇编语句与输出的机器码分开保存。
    def __init__(self):
        self.instructions = []
        self.labels = {}

    # Copy caller-owned operands so later mutations cannot change the program.
    # 复制调用方的操作数，避免后续修改改变程序。
    def emit(self, op, **operands):
        self.instructions.append((op, dict(operands)))
        return self

    # Labels identify instruction indices rather than byte addresses.
    # 标签标识指令索引，而非字节地址。
    def label(self, name):
        if name in self.labels:
            raise ValueError('duplicate label: ' + name)
        self.labels[name] = len(self.instructions)
        return self

    # Resolve labels before invoking the generated instruction encoder.
    # 调用生成的指令编码器之前先解析标签。
    def binary(self):
        output = bytearray()
        for op, original in self.instructions:
            operands = dict(original)
            if isinstance(operands.get('imm'), str):
                name = operands['imm']
                if name not in self.labels:
                    raise ValueError('unknown label: ' + name)
                operands['imm'] = self.labels[name]
            output.extend(encode(op, **operands))
        return bytes(output)

    # Save raw little-endian instructions for the C++ loader.
    # 保存原始小端指令，供 C++ 装载器读取。
    def save(self, path):
        Path(path).write_bytes(self.binary())

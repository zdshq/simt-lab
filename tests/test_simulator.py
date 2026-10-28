#!/usr/bin/env python3
# Compare actual machine-code execution with independently computed results.
# 将实际机器码执行结果与独立计算的参考值比较。
import importlib.util
import json
import math
from pathlib import Path
import random
import struct
import unittest
from support import SimulationCase

ROOT = Path(__file__).resolve().parents[1]
from simt_isa import encode, decode, OPS, FIELDS
from simt_lab import Program, f32, Z, NOT_PT

MASK = (1 << 32) - 1


# Interpret raw bits as a signed value in the Python reference.
# 在 Python 参考中将原始位模式解释为有符号数。
def signed(n):
    return n if n < (1 << 31) else n - (1 << 32)


class SimulationTest(SimulationCase):
    # Check fixed bit positions independently of the decoder.
    # 独立于解码器检查固定的编码位位置。
    def test_encoding_known_layout(self):
        raw = encode('MOVI', rd=9, imm=0x12345678, guard=10, control=0x12345)
        self.assertEqual(raw, struct.pack('<4I', 1 | (9 << 8), 10 << 8, 0x12345678, 0x12345 << 11))
        self.assertEqual(encode('MOVI', rd=1, imm=-1), encode('MOVI', rd=1, imm=MASK))

    # Exercise every opcode with deterministic randomized operand fields.
    # 使用确定性的随机操作数字段覆盖所有操作码。
    def test_encoding_roundtrip(self):
        rng = random.Random(8517)
        for name, op in OPS.items():
            for _ in range(40):
                values = {f: rng.randrange(1 << FIELDS[f]['bits']) for f in op['fields']}
                values.update(guard=rng.randrange(16), control=rng.randrange(1 << 21))
                with self.subTest(op=name):
                    decoded, operands = decode(encode(name, **values))
                    self.assertEqual(decoded, name)
                    self.assertEqual(operands, values)

    # Invalid schemas must fail before C++ and Python disagree about encoding.
    # 无效描述必须在 C++ 与 Python 编码产生分歧之前失败。
    def test_schema_rejects_overlap_and_duplicate_opcodes(self):
        spec = importlib.util.spec_from_file_location('gen_isa', ROOT / 'tools/gen_isa.py')
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        schema = json.loads((ROOT / 'isa/isa.json').read_text())
        schema['fields'][1]['shift'] = 0
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            generator.validate(schema)
        schema = json.loads((ROOT / 'isa/isa.json').read_text())
        schema['instructions'][1]['code'] = 0
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            generator.validate(schema)

    # Reject malformed binaries at the real executable boundary.
    # 在实际可执行文件入口拒绝畸形二进制。
    def test_invalid_encodings(self):
        for raw, error in [(b'', 'invalid_program_size'), (b'\0' * 15, 'invalid_program_size'),
                           (struct.pack('<4I', 255, 7 << 8, 0, 0), 'unknown_opcode'),
                           (struct.pack('<4I', 0, 7 << 8, 1, 0), 'reserved_encoding_bits'),
                           (struct.pack('<4I', 0, 7 << 8, 0, 1), 'reserved_encoding_bits')]:
            with self.subTest(error=error, raw=raw):
                self.execute(raw, error=error)
        with self.assertRaises(ValueError): encode('NOP', rd=1)
        with self.assertRaises(ValueError): encode('MOVI', rd=256)
        with self.assertRaises(ValueError): encode('MOVI', imm=1 << 32)
        with self.assertRaises(ValueError): decode(struct.pack('<4I', 0, 7 << 8, 0, 1))

    # Compare all lanes against Python integer arithmetic with explicit wrapping.
    # 将全部线程与显式取模的 Python 整数运算比较。
    def test_integer_programs_against_scalar_reference(self):
        functions = {
            'IADD': lambda a,b,c: a+b, 'ISUB': lambda a,b,c: a-b,
            'IMUL': lambda a,b,c: a*b, 'IMAD': lambda a,b,c: a*b+c,
            'AND': lambda a,b,c: a&b, 'OR': lambda a,b,c: a|b, 'XOR': lambda a,b,c: a^b,
            'SHL': lambda a,b,c: a << (b&31), 'SHR': lambda a,b,c: a >> (b&31),
            'SAR': lambda a,b,c: signed(a) >> (b&31),
        }
        rng = random.Random(20261029)
        for trial in range(8):
            base, stride, b, c = [rng.getrandbits(32) for _ in range(4)]
            for op, reference in functions.items():
                p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=base)
                p.emit('MOVI', rd=2, imm=stride).emit('IMAD', rd=1, ra=0, rb=2, rc=1)
                p.emit('MOVI', rd=2, imm=b).emit('MOVI', rd=3, imm=c)
                fields = dict(rd=4, ra=1, rb=2)
                if op == 'IMAD': fields['rc'] = 3
                p.emit(op, **fields).emit('EXIT')
                with self.subTest(trial=trial, op=op):
                    result = self.execute(p)
                    self.assertEqual(self.registers(result, 4),
                                     [reference((base + stride*l)&MASK,b,c)&MASK for l in range(32)])

    # Check block admission, two-SM placement, and a partially occupied warp.
    # 检查线程块准入、双 SM 派发和未占满的 warp。
    def test_two_sms_multiwarp_partial_warp_and_block_retirement(self):
        p = Program()
        for special in range(7): p.emit('S2R', rd=special, imm=special)
        p.emit('EXIT')
        result = self.execute(p, '--blocks', 5, '--threads', 37)
        self.assertEqual(result['sm_count'], 2)
        self.assertEqual(result['blocks_completed'], 5)
        self.assertEqual([c['sm_id'] for c in result['ctas']], [0,1,0,1,0])
        for c in result['ctas']:
            self.assertTrue(c['completed'])
            for wi,w in enumerate(c['warps']):
                active = min(32,37-32*wi)
                self.assertEqual(w['valid_mask'], (1<<active)-1)
                for l in range(32):
                    expected = [l, wi*32+l, wi,c['block_id'],c['sm_id'],37,5] if l<active else [0]*7
                    self.assertEqual([w['registers'][r][l] for r in range(7)], expected)
        self.assertEqual(result['lane_instructions'], 5*37*8)

    # Predicated-off instructions and zero-register writes have no side effects.
    # 谓词关闭的指令和零寄存器写入不应产生副作用。
    def test_predicates_and_zero_register(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16)
        p.emit('ISETP', rd=0, ra=0, rb=1, mode=2)
        p.emit('MOVI', rd=2, imm=10, guard=0).emit('MOVI', rd=2, imm=20, guard=8)
        p.emit('MOVI', rd=3, imm=50, guard=NOT_PT)
        p.emit('MOVI', rd=Z, imm=999).emit('MOV', rd=4, ra=Z).emit('EXIT')
        result = self.execute(p)
        self.assertEqual(self.registers(result,2), [10]*16+[20]*16)
        for r in [3,4,Z]: self.assertEqual(self.registers(result,r), [0]*32)

    # Retired lanes must not participate in later instructions.
    # 已退出线程不得参与后续指令。
    def test_predicated_exit_and_partial_predicate_write(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16)
        p.emit('ISETP', rd=0, ra=0, rb=1, mode=2).emit('EXIT', guard=0)
        p.emit('MOVI', rd=2, imm=123).emit('EXIT')
        result = self.execute(p)
        self.assertEqual(self.registers(result,2), [0]*16+[123]*16)

    # Cover unsigned and signed comparisons around the sign boundary.
    # 覆盖符号边界附近的无符号与有符号比较。
    def test_integer_comparison_modes(self):
        for a,b in [(MASK,0),(0,0),(0x80000000,1),(3,4),(4,3)]:
            expected = [a==b,a!=b,a<b,a<=b,signed(a)<signed(b),signed(a)<=signed(b)]
            p = Program().emit('MOVI',rd=0,imm=a).emit('MOVI',rd=1,imm=b)
            for mode in range(6): p.emit('ISETP',rd=mode,ra=0,rb=1,mode=mode)
            p.emit('EXIT')
            result = self.execute(p,'--threads',1)
            self.assertEqual(result['ctas'][0]['warps'][0]['predicates'][:6],list(map(int,expected)))

    # A cancellation case distinguishes fused from separately rounded multiply-add.
    # 使用抵消案例区分融合乘加与分别舍入的乘加。
    def test_floating_operations_and_fused_rounding(self):
        p = Program().emit('MOVI',rd=0,imm=f32(1+2**-23)).emit('MOVI',rd=1,imm=f32(1-2**-23))
        p.emit('MOVI',rd=2,imm=f32(-1)).emit('FFMA',rd=3,ra=0,rb=1,rc=2)
        p.emit('FMUL',rd=4,ra=0,rb=1).emit('FADD',rd=5,ra=4,rb=2)
        p.emit('MOVI',rd=6,imm=f32(4)).emit('FEXP2',rd=7,ra=6)
        p.emit('FRCP',rd=8,ra=6).emit('FRSQRT',rd=9,ra=6)
        p.emit('MOVI',rd=10,imm=-7).emit('I2F',rd=11,ra=10)
        p.emit('MOVI',rd=12,imm=f32(-7.75)).emit('F2I',rd=13,ra=12)
        p.emit('FSETP',rd=0,ra=2,rb=6,mode=2).emit('EXIT')
        result = self.execute(p,'--threads',1)
        for reg,val in {3:f32(-2**-46),4:f32(1),5:f32(0),7:f32(16),8:f32(.25),9:f32(.5),11:f32(-7),13:(-7)&MASK}.items():
            self.assertEqual(self.registers(result,reg)[0],val)
        self.assertEqual(result['ctas'][0]['warps'][0]['predicates'][0],1)

    # Functional execution must not assign timing meaning to control bits.
    # 功能执行不得为控制位赋予时序含义。
    def test_control_bits_decode_without_timing_effect(self):
        normal = Program().emit('MOVI',rd=1,imm=123).emit('EXIT')
        control = Program().emit('MOVI',rd=1,imm=123,control=(1<<21)-1).emit('EXIT')
        self.assertEqual(self.execute(normal),self.execute(control))

    # Failure diagnostics identify the virtual block, SM, warp, and PC.
    # 失败诊断应指出虚拟线程块、SM、warp 和 PC。
    def test_errors_are_contextual(self):
        p = Program().emit('MOVI',rd=64,imm=1).emit('EXIT')
        error = self.execute(p,error='register_out_of_range')['error']
        self.assertIn('block=0 sm=0 warp=0 pc=0',error)
        self.execute(Program().emit('NOP'),error='pc_out_of_range')
        self.execute(Program().emit('NOP').emit('EXIT'),'--max-steps',1,error='step_limit_exceeded')
        self.execute(Program().emit('EXIT'),'--sms',0,error='invalid_launch_config')
        p = Program().emit('MOVI',rd=0,imm=f32(math.inf)).emit('F2I',rd=1,ra=0).emit('EXIT')
        self.execute(p,error='float_to_integer_out_of_range')
        p = Program().emit('ISETP',rd=7,ra=0,rb=0).emit('EXIT')
        self.execute(p,error='predicate_out_of_range')


if __name__ == '__main__':
    unittest.main(verbosity=2)

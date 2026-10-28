#!/usr/bin/env python3
# Build a kernel exposing logical IDs and simulated SM placement.
# 构造展示逻辑标识和模拟 SM 派发位置的 kernel。
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


# Allow examples to use any configured build directory.
# 允许示例使用任意已配置的构建目录。
def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'identifiers.bin')
    return parser.parse_args()


# Generate a real machine-code program through the public assembler.
# 通过公开汇编接口生成真实的机器码程序。
def build_kernel():
    from simt_lab import Program
    program = Program()
    for special in range(7):
        program.emit('S2R', rd=special, imm=special)
    program.emit('MOVI', rd=8, imm=1000)
    program.emit('IMAD', rd=9, ra=3, rb=8, rc=1)
    return program.emit('EXIT')


# Configure generated imports and save the assembled kernel.
# 配置生成代码的导入路径，并保存汇编后的 kernel。
def run_application():
    options = parse_options()
    sys.path[:0] = [str(options.build / 'generated'), str(ROOT / 'python')]
    build_kernel().save(options.out)
    print(options.out)


if __name__ == '__main__':
    run_application()

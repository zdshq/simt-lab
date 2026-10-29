#!/usr/bin/env python3
# Save executable experiments and model traces for dependency, cache, and bank behavior.
# 保存依赖、缓存和 bank 行为的可执行实验与模型轨迹。
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--out', type=Path, default=ROOT/'artifacts'/'hardware')
    return parser.parse_args()


# Change only the operand dependency while keeping the instruction count constant.
# 保持指令数量相同，仅改变操作数依赖关系。
def arithmetic(dependent):
    from simt_lab import Program, Z
    program = Program()
    for index in range(12):
        program.emit('IADD', rd=0 if dependent else index, ra=0 if dependent else Z, rb=Z)
    return program.emit('EXIT')


def shared(stride):
    from simt_lab import Program
    program = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=stride)
    program.emit('IMUL', rd=2, ra=0, rb=1).emit('LDS', rd=3, ra=2, mode=4)
    return program.emit('EXIT')


# A younger independent move should retire before the cold load completes.
# 较新的独立 MOV 应在冷 load 完成前退休。
def cold_load():
    from simt_lab import Program, Z
    program = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('MOVI', rd=1, imm=9)
    program.emit('IADD', rd=2, ra=0, rb=1).emit('LDG', rd=3, ra=Z, mode=4)
    return program.emit('EXIT')


def execute(options, name, program):
    folder = options.out/name
    folder.mkdir(parents=True, exist_ok=True)
    path = folder/'kernel.bin'
    program.save(path)
    command = [str(options.build/'simt-run'), str(path), '--hardware-trace', '--dump-state']
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    report = json.loads(result.stdout)
    (folder/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    (folder/'command.json').write_text(json.dumps(command, indent=2)+'\n')
    return {'experiment': name, **{k: report['hardware'][k] for k in
            ('cycles', 'issued', 'retired', 'scoreboard_stalls', 'register_bank_stalls')}}


# All times printed here are clocks of this configured model, not RTX 4070 SUPER measurements.
# 此处打印的时间均为当前配置的模型时钟，不是 RTX 4070 SUPER 实测值。
def run_experiments(options):
    sys.path[:0] = [str(options.build/'generated'), str(ROOT/'python')]
    programs = {'dependent': arithmetic(True), 'independent': arithmetic(False),
                'shared_contiguous': shared(4), 'shared_conflict': shared(128),
                'shared_broadcast': shared(0), 'cold_load': cold_load()}
    summaries = [execute(options, name, program) for name, program in programs.items()]
    (options.out/'summary.json').write_text(json.dumps(summaries, indent=2)+'\n')
    for summary in summaries:
        print(json.dumps(summary))


def run_application():
    run_experiments(parse_options())


if __name__ == '__main__':
    run_application()

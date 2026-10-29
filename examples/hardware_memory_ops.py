#!/usr/bin/env python3
# Preserve local-memory and atomic-bank experiments as executable, validated artifacts.
# 将 local 内存和原子 bank 实验保存为可执行、经过验证的产物。
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--out', type=Path, default=ROOT/'artifacts'/'memory-ops')
    return parser.parse_args()


def local_program(stride):
    from simt_lab import Program
    p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=stride)
    p.emit('IMUL', rd=2, ra=0, rb=1).emit('LDL', rd=4, ra=2, mode=4)
    return p.emit('LDL', rd=5, ra=2, mode=4).emit('EXIT')


def atomic_program(space, stride):
    from simt_lab import Program
    p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=stride)
    p.emit('IMUL', rd=2, ra=0, rb=1).emit('MOVI', rd=3, imm=1)
    return p.emit('ATOM', rd=4, ra=2, rb=3, aux=space).emit('EXIT')


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


# Check returned values and actual backing words rather than accepting timing counters alone.
# 检查返回值及实际底层字，不能只接受计时计数。
def validate(report, kind, stride):
    expected = list(range(32)) if kind != 'local' and stride == 0 else [0]*32
    if report['ctas'][0]['warps'][0]['registers'][4] != expected:
        raise RuntimeError('incorrect old values: '+kind)
    if kind == 'local':
        return
    data = dict(report['global'] if kind == 'global' else report['ctas'][0]['shared'])
    for lane in range(32):
        value = sum(data.get(lane*stride+byte, 0) << (8*byte) for byte in range(4))
        if value != (32 if stride == 0 else 1):
            raise RuntimeError('incorrect atomic result: '+kind)


def execute(options, kind, stride):
    name = kind+'_'+str(stride)
    folder = options.out/name
    folder.mkdir(parents=True, exist_ok=True)
    program = local_program(stride) if kind == 'local' else atomic_program(int(kind == 'shared'), stride)
    program.save(folder/'kernel.bin')
    command = [str(options.build/'simt-run'), str(folder/'kernel.bin'), '--timing', '--hardware-trace', '--dump-state', '--dump-memory']
    save_json(folder/'command.json', command)
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    report = json.loads(result.stdout)
    save_json(folder/'result.json', report)
    validate(report, kind, stride)
    hardware = report['hardware']
    summary = {'experiment': name, 'model_cycles': hardware['cycles'], 'verified_lanes': 32,
               'dram': hardware['dram'], 'l1_hits': hardware['l1'][0]['hits'],
               'global_atomic': hardware['global_atomic'], 'shared': hardware['shared'][0]}
    save_json(folder/'validation.json', summary)
    return summary


def run_experiments(options):
    sys.path[:0] = [str(options.build/'generated'), str(ROOT/'python')]
    cases = [('local', 0), ('local', 4), ('global', 0), ('global', 32), ('shared', 0), ('shared', 4)]
    summaries = [execute(options, *case) for case in cases]
    save_json(options.out/'summary.json', summaries)
    for summary in summaries:
        print(json.dumps(summary))


def run_application():
    run_experiments(parse_options())


if __name__ == '__main__':
    run_application()

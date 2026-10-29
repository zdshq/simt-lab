#!/usr/bin/env python3
# Retain byte-verified DMA experiments and complete model traces.
# 保存经过逐字节验证的 DMA 实验及完整模型轨迹。
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--out', type=Path, default=ROOT/'artifacts'/'async')
    return parser.parse_args()


# One elected lane submits commands; all lanes wait before consuming their destination.
# 一个选定 lane 提交命令，全部 lane 在消费目标数据前等待。
def kernel(size, copies):
    from simt_lab import Program, Z
    p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z)
    p.emit('MOVI', rd=1, imm=1024).emit('MB_INIT', ra=1, imm=1, guard=0)
    p.emit('MB_EXPECT', ra=1, imm=size*copies, guard=0).emit('MB_ARRIVE', rd=3, ra=1, guard=0)
    for index in range(copies):
        p.emit('MOVI', rd=4, imm=64+index*size).emit('MOVI', rd=2, imm=8192+index*size)
        p.emit('CP_ASYNC', ra=4, rb=2, rc=1, imm=size, guard=0)
    p.emit('MOVI', rd=9, imm=123).emit('MB_WAIT', ra=1, rb=3)
    return p.emit('LDS', rd=8, ra=2, mode=4).emit('EXIT')


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


# Validate actual shared bytes and expose the difference between submission and completion.
# 验证实际 shared 字节，并展示提交与完成的区别。
def validate(report, data, name):
    memory = dict(report['ctas'][0]['shared'])
    copied = bytes(memory.get(8192+offset, 0) for offset in range(len(data)-64))
    if copied != data[64:]:
        raise RuntimeError('DMA payload mismatch: '+name)
    trace = report['hardware']['trace']
    submitted = [event for event in trace if event['action'] == 'copy_submitted']
    first = submitted[0]['instruction']
    retired = next(event['cycle'] for event in trace if event['instruction'] == first and event['action'] == 'retire')
    complete = next(event['cycle'] for event in trace if event['instruction'] == first and event['action'] == 'copy_complete')
    if retired >= complete:
        raise RuntimeError('Expected the submitted DMA to outlive its instruction')
    return {'experiment': name, 'verified_bytes': len(copied), 'model_cycles': report['hardware']['cycles'],
            'first_cp_retire': retired, 'first_copy_complete': complete, **report['hardware']['async'][0]}


def execute(options, name, size, copies, arguments):
    folder = options.out/name
    folder.mkdir(parents=True, exist_ok=True)
    kernel(size, copies).save(folder/'kernel.bin')
    data = bytes((offset*37+9) % 256 for offset in range(64+size*copies))
    (folder/'global.bin').write_bytes(data)
    command = [str(options.build/'simt-run'), str(folder/'kernel.bin'), '--timing', '--hardware-trace',
               '--dump-state', '--dump-memory', '--global', str(folder/'global.bin'), *arguments]
    save_json(folder/'command.json', command)
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    report = json.loads(result.stdout)
    save_json(folder/'result.json', report)
    summary = validate(report, data, name)
    save_json(folder/'validation.json', summary)
    return summary


# Change one bounded resource at a time; all reported clocks belong to this research model.
# 每次只改变一种有限资源，全部时钟均属于本研究模型。
def run_experiments(options):
    sys.path[:0] = [str(options.build/'generated'), str(ROOT/'python')]
    cases = [(f'fragments_{count}', 4096, 1, ['--async-fragments', str(count)]) for count in (1, 4, 8)]
    cases += [(f'commands_{count}', 256, 4, ['--async-depth', str(count)]) for count in (1, 8)]
    summaries = [execute(options, *case) for case in cases]
    save_json(options.out/'summary.json', summaries)
    for summary in summaries:
        print(json.dumps(summary))


def run_application():
    run_experiments(parse_options())


if __name__ == '__main__':
    run_application()

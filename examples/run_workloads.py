#!/usr/bin/env python3
# Run compositional examples and retain their complete reproduction artifacts.
# 运行组合示例，并保留完整复现产物。
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


# Keep example configuration separate from workload construction.
# 将示例配置与工作负载构造分开。
def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build',type=Path,default=ROOT/'build')
    parser.add_argument('--out',type=Path,default=ROOT/'artifacts'/'examples')
    parser.add_argument('--workload',choices=['all','vector','reduction','gemm','transformer'],default='all')
    parser.add_argument('--policy',choices=['rr','min','max'],default='rr')
    return parser.parse_args()


# Import the generated ISA only after selecting the requested build directory.
# 选择构建目录之后再导入生成的 ISA。
def execute_examples(options):
    sys.path[:0] = [str(options.build/'generated'),str(ROOT/'python')]
    from simt_lab.workloads import vector_add, reduction, gemm
    from simt_lab.transformer import tiny_transformer
    from simt_lab.runtime import run_workload
    builders = {'vector':vector_add,'reduction':reduction,'gemm':gemm,'transformer':tiny_transformer}
    selected = builders if options.workload=='all' else {options.workload:builders[options.workload]}
    for name,builder in selected.items():
        summary = run_workload(builder(),options.build/'simt-run',options.out/name,options.policy)
        print(json.dumps(summary,ensure_ascii=False))


# Coordinate parsing and execution without embedding work in the entry point.
# 组织参数解析和执行，不将具体工作放进入口。
def run_application():
    execute_examples(parse_options())


if __name__ == '__main__':
    run_application()

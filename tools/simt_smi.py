#!/usr/bin/env python3
# Launch the unmodified NVIDIA program against the simulator's process-local NVML backend.
# 使用模拟器进程范围内的 NVML 后端启动未修改的 NVIDIA 程序。
import argparse
import os
from pathlib import Path
import subprocess
import sys


def options():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1]/'build')
    parser.add_argument('--registry', type=Path)
    return parser.parse_known_args()


# Only the child process receives the library override; system libraries remain intact.
# 仅子进程接收库路径覆盖，系统库保持不变。
def environment(arguments):
    result = dict(os.environ)
    library = arguments.build.resolve()/'nvml'
    if not (library/'libnvidia-ml.so.1').is_file():
        raise RuntimeError('build simt_nvml first; NVML headers and the native library are required')
    result['LD_LIBRARY_PATH'] = str(library)+(':'+result['LD_LIBRARY_PATH'] if result.get('LD_LIBRARY_PATH') else '')
    if arguments.registry is not None:
        result['SIMT_MONITOR_DIR'] = str(arguments.registry.resolve())
    return result


def run_application():
    arguments, forwarded = options()
    try:
        return subprocess.run(['/usr/bin/nvidia-smi',*forwarded],env=environment(arguments)).returncode
    except (OSError,RuntimeError) as error:
        print(str(error),file=sys.stderr)
        return 1


def main():
    raise SystemExit(run_application())


if __name__ == '__main__':
    main()

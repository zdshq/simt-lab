#!/usr/bin/env python3
# Launch the unmodified NVIDIA program against the simulator's process-local NVML backend.
# 使用模拟器进程范围内的 NVML 后端启动未修改的 NVIDIA 程序。
import argparse
import os
from pathlib import Path
import subprocess
import sys


def options():
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1]/'build')
    parser.add_argument('--registry', type=Path)
    parser.add_argument('--merge', action='store_true')
    parser.add_argument('--auto', action='store_true')
    return parser.parse_known_args()


# Only the child process receives the library override; system libraries remain intact.
# 仅子进程接收库路径覆盖，系统库保持不变。
def environment(arguments):
    result = dict(os.environ)
    library = arguments.build.resolve()/'nvml'
    if not (library/'libnvidia-ml.so.1').is_file():
        raise RuntimeError('build simt_nvml first; NVML headers and the native library are required')
    result['LD_LIBRARY_PATH'] = str(library)+(':'+result['LD_LIBRARY_PATH'] if result.get('LD_LIBRARY_PATH') else '')
    result['SIMT_NVML_MERGE'] = '1' if arguments.merge else '0'
    if arguments.registry is not None:
        result['SIMT_MONITOR_DIR'] = str(arguments.registry.resolve())
    return result


# The installed command merges supported reports; administration and other modes use native NVML.
# 安装后的命令合并已支持的报告；管理操作和其他模式使用原生 NVML。
def supported_query(arguments):
    switches = {'-L', '--list-gpus', '-q', '--query', '-x', '--xml-format'}
    values = {'-i', '--id', '-d', '--display', '-l', '--loop', '--loop-ms', '-f', '--filename',
              '--format', '--query-gpu'}
    pending = None
    for argument in arguments:
        if pending:
            if pending in ('-i', '--id') and ':' in argument: return False
            pending = None
            continue
        name, separator, value = argument.partition('=')
        if name in values:
            if name in ('-i', '--id') and ':' in value: return False
            pending = name if not separator else None
        elif name not in switches or separator: return False
    return not pending


def native_environment(arguments):
    result = dict(os.environ)
    bridge = arguments.build.resolve()/'nvml'
    paths = [path for path in result.get('LD_LIBRARY_PATH', '').split(':') if path and Path(path).resolve() != bridge]
    if paths: result['LD_LIBRARY_PATH'] = ':'.join(paths)
    else: result.pop('LD_LIBRARY_PATH', None)
    result.pop('SIMT_NVML_MERGE', None)
    return result


def run_application():
    arguments, forwarded = options()
    try:
        merged_command = arguments.auto or arguments.merge
        env = environment(arguments) if not merged_command or supported_query(forwarded) else native_environment(arguments)
        return subprocess.run(['/usr/bin/nvidia-smi',*forwarded],env=env).returncode
    except (OSError,RuntimeError) as error:
        print(str(error),file=sys.stderr)
        return 1


def main():
    raise SystemExit(run_application())


if __name__ == '__main__':
    main()

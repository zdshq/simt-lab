#!/usr/bin/env python3
# Keep an actual simulator session alive so another terminal can query it with nvidia-smi.
# 保持实际模拟器会话存活，让另一终端能通过 nvidia-smi 查询。
import argparse
import os
from pathlib import Path
import sys
import time


def options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1]/'build')
    parser.add_argument('--registry', type=Path, default=Path('/tmp')/f'simt-lab-{os.getuid()}')
    parser.add_argument('--seconds', type=float, default=120)
    parser.add_argument('--bytes', type=int, default=7*1024*1024)
    parser.add_argument('--timing', action='store_true')
    parser.add_argument('--tensor-rtl', action='store_true')
    return parser.parse_args()


def configure_imports(build):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
    sys.path.insert(0,str(build.resolve()/'generated'))


# Allocation and upload are real device operations; the registry merely publishes their state.
# 分配和上传均为实际设备操作，注册目录只发布它们的状态。
def demonstrate(arguments):
    from simt_lab.host import Device
    with Device(arguments.build/'simt-device',monitor_dir=arguments.registry,
                timing=arguments.timing,tensor_rtl=arguments.tensor_rtl) as device:
        buffer = device.allocate(arguments.bytes)
        device.h2d(buffer,b'SIMT').wait()
        print(f'Live simulator PID {device.connection.process.pid}; registry {arguments.registry}',flush=True)
        print('Run: python3 tools/simt_smi.py -L',flush=True)
        deadline = time.monotonic()+arguments.seconds
        while time.monotonic() < deadline:
            time.sleep(min(1,max(0,deadline-time.monotonic())))


def run_application():
    arguments = options()
    configure_imports(arguments.build)
    try:
        demonstrate(arguments)
        return 0
    except KeyboardInterrupt:
        return 0
    except (OSError,RuntimeError) as error:
        print(str(error),file=sys.stderr)
        return 1


def main():
    raise SystemExit(run_application())


if __name__ == '__main__':
    main()

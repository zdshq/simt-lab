#!/usr/bin/env python3
# Preserve a private tool installation's root on both configure-time and build-time invocations.
# 在配置和构建阶段调用时，均保留私有工具安装目录。
import os
from pathlib import Path
import sys


def run_application():
    root, executable = Path(str(Path(__file__).resolve())+'.config').read_text().splitlines()
    environment = dict(os.environ, VERILATOR_ROOT=root)
    os.execve(executable, [executable, *sys.argv[1:]], environment)


if __name__ == '__main__':
    run_application()

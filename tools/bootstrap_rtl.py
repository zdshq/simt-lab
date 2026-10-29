#!/usr/bin/env python3
# Install the pinned Ubuntu toolchain into the project without sudo or system-library changes.
# 将固定版本的 Ubuntu 工具链放入项目，不使用 sudo 或修改系统库。
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT/'.tools/rtl'
YOSYS_COMMIT = 'a1bb0255d654ffd0d8503577274f24b349a42995'
PACKAGES = ['verilator=4.038-1', 'iverilog=11.0-1.1', 'libboost1.74-dev=1.74.0-14ubuntu3',
            'berkeley-abc=1.01+20211229git48498af+dfsg-2']


def command(arguments, directory, **options):
    return subprocess.run(arguments, cwd=directory, check=True, **options)


def check_platform():
    release = Path('/etc/os-release').read_text()
    if 'ID=ubuntu' not in release or 'VERSION_ID="22.04"' not in release:
        raise RuntimeError('Pinned bootstrap targets Ubuntu 22.04; supply installed tools on other systems.')
    architecture = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
    if architecture != 'amd64': raise RuntimeError('Pinned bootstrap targets amd64.')


# APT authenticates archive metadata; extracted packages and their hashes remain inspectable.
# APT 验证归档元数据；解包后的软件包及其哈希保留供检查。
def prepare_packages():
    packages, root = TOOLS/'packages', TOOLS/'root'
    packages.mkdir(parents=True, exist_ok=True)
    root.mkdir(parents=True, exist_ok=True)
    command(['apt-get', 'download', *PACKAGES], packages)
    records = []
    for package in sorted(packages.glob('*.deb')):
        if package.name.startswith('yosys_'): continue
        command(['dpkg-deb', '-x', str(package), str(root)], ROOT)
        records.append({'file': package.name, 'sha256': hashlib.sha256(package.read_bytes()).hexdigest()})
    for name in ('verilator_bin', 'verilator_bin_dbg'):
        target = root/'usr/share/verilator/bin'/name
        if not target.exists(): target.symlink_to('../../../bin/'+name)
    return records


def prepare_yosys(jobs):
    source = TOOLS/'yosys-src'
    if not source.exists():
        command(['git', 'clone', '--depth', '1', '--branch', 'yosys-0.40',
                 'https://github.com/YosysHQ/yosys.git', str(source)], ROOT)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    if commit != YOSYS_COMMIT: raise RuntimeError('Local Yosys checkout differs from the pinned commit.')
    env = dict(os.environ, CXXFLAGS='-I'+str(TOOLS/'root/usr/include'))
    with (TOOLS/'yosys-build.log').open('w') as log:
        command(['make', f'-j{jobs}', 'ENABLE_TCL=0', 'ENABLE_READLINE=0',
                 'ABCEXTERNAL='+str(TOOLS/'root/usr/bin/berkeley-abc')], source,
                env=env, stdout=log, stderr=subprocess.STDOUT)
    return subprocess.check_output([str(source/'yosys'), '-V'], text=True).strip()


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jobs', type=int, default=6)
    return parser.parse_args()


def run_application():
    options = parse_options()
    check_platform()
    packages = prepare_packages()
    version = prepare_yosys(options.jobs)
    (TOOLS/'manifest.json').write_text(json.dumps({'packages': packages, 'yosys_commit': YOSYS_COMMIT,
                                                  'yosys_version': version}, indent=2)+'\n')
    print(version)
    print('Ready: cmake --build build --target rtl-check')


if __name__ == '__main__':
    run_application()

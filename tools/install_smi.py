#!/usr/bin/env python3
# Install a reversible per-user command that executes the original NVIDIA binary through the bridge.
# 安装可撤销的用户级命令，通过桥接层执行原版 NVIDIA 二进制。
import argparse
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKER = '# simt_smi_config: '


def options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--prefix', type=Path, default=Path.home()/'.local/bin')
    parser.add_argument('--uninstall', action='store_true')
    return parser.parse_args()


def script(config):
    return ('#!/usr/bin/env python3\n'+MARKER+json.dumps(config, sort_keys=True)+'\n'
            '# Run the native NVIDIA program through the user-scoped query bridge.\n'
            '# 通过用户范围的查询桥接层运行原版 NVIDIA 程序。\n'
            'import os\nimport sys\nfrom pathlib import Path\n\n\n'
            'def run():\n'
            f'    launcher = {config["launcher"]!r}\n'
            f'    build = {config["build"]!r}\n'
            '    if Path(launcher).is_file() and (Path(build)/"nvml/libnvidia-ml.so.1").is_file():\n'
            '        os.execv(sys.executable, [sys.executable, launcher, "--build", build, "--merge", "--auto", *sys.argv[1:]])\n'
            '    os.execv("/usr/bin/nvidia-smi", ["/usr/bin/nvidia-smi", *sys.argv[1:]])\n\n\n'
            'if __name__ == "__main__":\n    run()\n')


# Never replace an unrelated command, a symlink, or a user-modified installation.
# 不替换无关命令、符号链接或用户修改过的已安装脚本。
def check_existing(target):
    if target.is_symlink(): raise RuntimeError(f'refusing symlink: {target}')
    if not target.exists(): return
    text = target.read_text()
    try:
        line = text.splitlines()[1]
        if not line.startswith(MARKER): raise ValueError('missing marker')
        config = json.loads(line[len(MARKER):])
        if config['launcher'] != str(ROOT/'tools/simt_smi.py') or text != script(config):
            raise ValueError('modified or foreign launcher')
    except (IndexError, KeyError, TypeError, ValueError) as error:
        raise RuntimeError(f'refusing unrelated or modified command: {target}') from error


def replace_file(target, text):
    with tempfile.NamedTemporaryFile(mode='w', dir=target.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(text)
    try:
        temporary.chmod(0o755)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def install(arguments):
    target = arguments.prefix.expanduser().absolute()/'nvidia-smi'
    check_existing(target)
    if arguments.uninstall:
        target.unlink(missing_ok=True)
        print(f'Removed SIMT command: {target}')
        return
    build = arguments.build.resolve()
    if not (build/'nvml/libnvidia-ml.so.1').is_file(): raise RuntimeError('build simt_nvml first')
    target.parent.mkdir(parents=True, exist_ok=True)
    replace_file(target, script({'launcher':str(ROOT/'tools/simt_smi.py'), 'build':str(build)}))
    print(f'Installed: {target}; native binary: /usr/bin/nvidia-smi')


def main():
    install(options())


if __name__ == '__main__':
    main()

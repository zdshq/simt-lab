# Unsupported public NVML functions preserve their official ABI and report lack of support.
# 未支持的公开 NVML 函数保留官方 ABI，并报告不支持。
import argparse
from pathlib import Path
import re


def options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--header', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    return parser.parse_args()


# Read declarations rather than generating mismatched variadic stubs for unknown signatures.
# 读取声明，不为未知签名生成不匹配的可变参数桩。
def declarations(header):
    pattern = r'nvmlReturn_t\s+DECLDIR\s+(nvml\w+)\s*\(([^;]*?)\)\s*;'
    return dict(re.findall(pattern, header, flags=re.DOTALL))


def generate(arguments):
    implemented = set()
    for source in arguments.source.glob('*.cpp'):
        implemented.update(re.findall(r'nvmlReturn_t\s+(nvml\w+)\s*\(', source.read_text()))
    lines = ['#define NVML_NO_UNVERSIONED_FUNC_DEFS', '#include <nvml.h>', '#include <cstdio>',
             '#include <cstdlib>', 'extern "C" {']
    for name, parameters in sorted(declarations(arguments.header.read_text()).items()):
        if name not in implemented:
            lines.extend([f'nvmlReturn_t {name}({parameters}) {{',
                          f'    if (std::getenv("SIMT_NVML_TRACE")) std::fprintf(stderr,"unsupported: {name}\\n");',
                          '    return NVML_ERROR_NOT_SUPPORTED;', '}'])
    lines.append('}')
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_text('\n'.join(lines)+'\n')


def main():
    generate(options())


if __name__ == '__main__':
    main()

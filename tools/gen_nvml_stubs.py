# Public NVML queries preserve official signatures and dispatch validated native handles.
# 公开 NVML 查询保留官方签名，并分派经过验证的原生句柄。
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


def argument_names(parameters):
    if parameters.strip() in ('', 'void'): return []
    return [re.search(r'(\w+)\s*(?:\[[^\]]*\])?\s*$', item).group(1) for item in parameters.split(',')]


# Opaque handle outputs and multi-device APIs need explicit translation, not generic forwarding.
# 不透明句柄输出和多设备 API 需要显式转换，不能直接通用转发。
def device_query(name, parameters):
    excluded = ('nvmlGpuInstance_t', 'nvmlComputeInstance_t', 'nvmlGpmSample_t', 'nvmlEventSet_t')
    return (name.startswith(('nvmlDeviceGet', 'nvmlDeviceIs')) and parameters.strip().startswith('nvmlDevice_t device,')
            and parameters.count('nvmlDevice_t') == 1 and not any(kind in parameters for kind in excluded))


def function_body(name, parameters):
    if device_query(name, parameters):
        args = ', '.join(['native']+argument_names(parameters)[1:])
        return ['    return simt::nvml::native_query(device, [&](nvmlDevice_t native) {',
                f'        return simt::nvml::native_call<decltype(&{name})>("{name}", {args});', '    });']
    if name in ('nvmlSystemGetCudaDriverVersion', 'nvmlSystemGetCudaDriverVersion_v2', 'nvmlSystemGetProcessName'):
        args = ', '.join(argument_names(parameters))
        return ['    return simt::nvml::system_query([&] {',
                f'        return simt::nvml::native_call<decltype(&{name})>("{name}", {args});',
                '    }, [] { return NVML_ERROR_NOT_SUPPORTED; });']
    return [f'    if (std::getenv("SIMT_NVML_TRACE")) std::fprintf(stderr,"unsupported: {name}\\n");',
            '    return NVML_ERROR_NOT_SUPPORTED;']


def native_reentry(name, parameters):
    args = ', '.join(argument_names(parameters))
    suffix = ', '+args if args else ''
    return ['    if (simt::nvml::inside_native_call())',
            f'        return simt::nvml::native_call<decltype(&{name})>("{name}"{suffix});']


def generate(arguments):
    implemented = set()
    for source in arguments.source.glob('*.cpp'):
        implemented.update(re.findall(r'nvmlReturn_t\s+(nvml\w+)\s*\(', source.read_text()))
    lines = ['#include "backend.hpp"', '#include <cstdio>',
             '#include <cstdlib>', 'extern "C" {']
    for name, parameters in sorted(declarations(arguments.header.read_text()).items()):
        if name not in implemented:
            lines.extend([f'nvmlReturn_t {name}({parameters}) {{', *native_reentry(name, parameters),
                          *function_body(name, parameters), '}'])
    lines.append('}')
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_text('\n'.join(lines)+'\n')


def main():
    generate(options())


if __name__ == '__main__':
    main()

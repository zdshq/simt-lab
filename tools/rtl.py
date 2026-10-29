#!/usr/bin/env python3
# Build real RTL simulations and synthesis artifacts without modifying system tools.
# 构建实际 RTL 仿真和综合产物，不修改系统工具。
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MODULES = {'cache': ('sector_cache', 'test_cache.cpp', ['sector_sram.sv', 'sector_cache.sv']),
           'scoreboard': ('lane_scoreboard', 'test_scoreboard.cpp', ['lane_scoreboard.sv']),
           'arbiter': ('register_arbiter', 'test_arbiter.cpp', ['register_arbiter.sv']),
           'fma': ('fp32_fma', 'test_fma.cpp', ['fp32_fma.sv']),
           'decode': ('tensor_decode', 'test_decode.cpp', ['tensor_decode.sv']),
           'pe': ('tensor_pe', 'test_pe.cpp', ['fp32_fma.sv', 'int8_mac.sv', 'tensor_pe.sv']),
           'tensor': ('tensor_array', 'test_array.cpp',
                      ['fp32_fma.sv', 'int8_mac.sv', 'tensor_decode.sv', 'tensor_pe.sv', 'tensor_array.sv'])}


def tool(name):
    override = os.environ.get('SIMT_'+name.upper().replace('-', '_'))
    local = ROOT/'.tools/rtl/root/usr/bin'/name
    if name == 'yosys': local = ROOT/'.tools/rtl/yosys-src/yosys'
    result = override or shutil.which(name) or (str(local) if local.is_file() else None)
    if result is None: raise RuntimeError(f'{name} missing; see docs/RTL.md')
    return str(Path(result).resolve())


def environment():
    result = dict(os.environ)
    local = ROOT/'.tools/rtl/root/usr/share/verilator'
    if Path(tool('verilator')).is_relative_to(ROOT/'.tools'):
        result['VERILATOR_ROOT'] = str(local)
    return result


# Keep complete diagnostics on disk and expose failures instead of silently skipping unavailable tools.
# 将完整诊断保存在磁盘，并报告失败，不静默跳过缺失工具。
def run(command, destination, name, env=None):
    path = destination/f'{name}.log'
    with path.open('w') as output:
        try:
            result = subprocess.run(command, cwd=destination, env=env, text=True, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=600)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f'{name} timed out; partial diagnostics: {path}') from error
    report = path.read_text()
    if result.returncode:
        raise RuntimeError(f'{name} failed ({result.returncode}):\n{report[-6000:]}')
    return report


def source_hashes(paths):
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}


def compiler_flags(options):
    includes = f'-std=c++20 -I{ROOT / "include"} -I{ROOT / "tests/rtl"} -I{options.build / "generated"} -DSIMT_RTL_SETS={options.sets}'
    libraries = ' '.join(str(options.build/name) for name in ('libsimt_core.a', 'libsimt_hardware.a', 'libsimt_storage.a'))
    if 'SIMT_TENSOR_RTL:BOOL=ON' in (options.build/'CMakeCache.txt').read_text():
        libraries += ' '+str(options.build/'libsimt_tensor_circuit.a')
    if 'SIMT_CACHE_RTL:BOOL=ON' in (options.build/'CMakeCache.txt').read_text():
        libraries += ' '+str(options.build/'libsimt_cache_circuit.a')
    if sanitized(options):
        includes += ' -fsanitize=address,undefined -fno-omit-frame-pointer'
        libraries += ' -fsanitize=address,undefined'
    return includes, libraries


def sanitized(options):
    cache = options.build/'CMakeCache.txt'
    return cache.is_file() and 'SIMT_SANITIZE:BOOL=ON' in cache.read_text()


def execute_simulation(options, destination, executable, label):
    env = dict(os.environ)
    env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '')+':halt_on_error=1'
    return run([str(executable)], destination, label, env)


def simulation_sources(options, name, destination):
    _, test, files = MODULES[name]
    if options.gate_sim:
        return ['-Wno-DECLFILENAME', '-Wno-UNUSED', str(destination/'gates.v'), str(options.simcells),
                str(ROOT/'tests/rtl'/test)]
    parameters = [f'-GSETS={options.sets}'] if name == 'cache' else []
    return parameters+[str(ROOT/'rtl'/file) for file in files]+[str(ROOT/'tests/rtl'/test)]


def simulation(options, name):
    top, _, files = MODULES[name]
    destination = options.out/name
    destination.mkdir(parents=True, exist_ok=True)
    if name == 'fma':
        run([sys.executable, str(ROOT/'tests/rtl/fma_vectors.py'), '--out', str(destination/'reference.txt')],
            destination, 'reference')
    if options.gate_sim: verify_netlist(destination, files)
    label, objects = ('gate', 'gate-obj') if options.gate_sim else ('simulation', 'obj')
    flags, libraries = compiler_flags(options)
    command = [tool('verilator'), '--cc', '--exe', '--build', '-j', str(options.jobs),
               '-Wall', '--unroll-count', '4096', '--top-module', top, '--Mdir', str(destination/objects),
               '-CFLAGS', flags, '-LDFLAGS', libraries]+simulation_sources(options, name, destination)
    if not sanitized(options): command.append('--trace')
    run(command, destination, label+'-build', environment())
    output = execute_simulation(options, destination, destination/objects/('V'+top), label)
    print(output, end='', flush=True)
    return {'checks': len([line for line in output.splitlines() if line.startswith('PASS ')]),
            'command': command, 'sources': source_hashes([ROOT/'rtl'/file for file in files]),
            'waveforms': [] if sanitized(options) else sorted(path.name for path in destination.glob('*.vcd'))}


def yosys_string(value):
    return '"'+str(value).replace('\\', '\\\\').replace('"', '\\"')+'"'


# Gate tests must use a netlist generated from the current RTL, not a stale successful artifact.
# 门级测试必须使用当前 RTL 生成的网表，不能沿用陈旧的成功产物。
def verify_netlist(destination, files):
    manifest = destination/'netlist.json'
    if not manifest.is_file(): raise RuntimeError('Run --synth before --gate-sim.')
    record = json.loads(manifest.read_text())
    if record['sources'] != source_hashes([ROOT/'rtl'/file for file in files]):
        raise RuntimeError('RTL changed since synthesis; rerun --synth.')
    digest = hashlib.sha256((destination/'gates.v').read_bytes()).hexdigest()
    if digest != record['gate_sha256']: raise RuntimeError('Gate netlist differs from synthesis manifest.')


# Retain memory macros for storage accounting before mapping the small configuration to generic gates.
# 先保留存储宏以统计存储容量，再将小配置映射为通用门单元。
def synthesis(options, name):
    top, _, files = MODULES[name]
    destination = options.out/name
    destination.mkdir(parents=True, exist_ok=True)
    lines = ['read_verilog -sv '+' '.join(yosys_string(ROOT/'rtl'/file) for file in files)]
    if name == 'cache': lines.append(f'chparam -set SETS {options.sets} -set WAYS {options.ways} {top}')
    lines += [f'synth -top {top} -run begin:fine', 'check -assert',
              'write_json inferred.json', 'tee -o inferred-stat.json stat -json']
    if not options.infer_only:
        lines += [f'synth -top {top} -run fine: -noabc',
                  'abc -exe '+yosys_string(tool('berkeley-abc'))+' -fast', 'clean', 'check -assert',
                  'write_json gates.json', 'tee -o gates-stat.json stat -json',
                  'clean -purge', 'splitnets', 'clean -purge', 'write_verilog -noattr gates.v']
    script = destination/'synthesis.ys'
    script.write_text('\n'.join(lines)+'\n')
    run([tool('yosys'), '-Q', '-T', '-s', str(script)], destination, 'synthesis')
    inferred = json.loads((destination/'inferred.json').read_text())
    memories = memory_inventory(inferred)
    result = {'top': top, 'memories': memories, 'inferred_stat': 'inferred-stat.json',
              'sources': source_hashes([ROOT/'rtl'/file for file in files])}
    if not options.infer_only:
        result['gate_stat'] = 'gates-stat.json'
        result['gate_sha256'] = hashlib.sha256((destination/'gates.v').read_bytes()).hexdigest()
        (destination/'netlist.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'synthesis': name, **result}), flush=True)
    return result


def memory_inventory(netlist):
    memories = []
    for module, definition in netlist['modules'].items():
        for name, cell in definition['cells'].items():
            if cell['type'] != '$mem_v2': continue
            parameters = cell['parameters']
            entry = {key.lower(): int(parameters[key], 2) for key in ('WIDTH', 'SIZE', 'RD_PORTS', 'WR_PORTS')}
            entry.update(module=module, name=name, bits=entry['width']*entry['size'])
            memories.append(entry)
    return memories


def save_report(options, label, results):
    executable = tool('yosys' if options.synth else 'verilator')
    version = run([executable, '-V' if options.synth else '--version'], options.out, label+'-version',
                  None if options.synth else environment())
    results['_tool'] = {'executable': executable, 'version': version.strip()}
    (options.out/(label+'.json')).write_text(json.dumps(results, indent=2)+'\n')


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--out', type=Path, default=ROOT/'artifacts/rtl')
    parser.add_argument('--module', choices=tuple(MODULES)+('all',), default='all')
    parser.add_argument('--sets', type=int, default=4)
    parser.add_argument('--ways', type=int, choices=(4, 8), default=4)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--synth', action='store_true')
    parser.add_argument('--infer-only', action='store_true')
    parser.add_argument('--gate-sim', action='store_true')
    parser.add_argument('--simcells', type=Path, default=ROOT/'.tools/rtl/yosys-src/techlibs/common/simcells.v')
    options = parser.parse_args()
    options.build, options.out = options.build.resolve(), options.out.resolve()
    options.simcells = options.simcells.resolve()
    if options.sets < 2 or options.sets > 256 or options.sets & (options.sets-1):
        parser.error('--sets must be a power of two in [2, 256]')
    if options.ways != 4 and not (options.module == 'cache' and options.synth and options.infer_only):
        parser.error('--ways 8 is supported for cache storage inference; full L2 execution is checked by CTest')
    if options.infer_only and not options.synth: parser.error('--infer-only requires --synth')
    if options.gate_sim and options.synth: parser.error('--gate-sim and --synth are separate steps')
    return options


def run_application():
    options = parse_options()
    names = MODULES if options.module == 'all' else [options.module]
    action = synthesis if options.synth else simulation
    results = {name: action(options, name) for name in names}
    label = 'synthesis' if options.synth else 'gate' if options.gate_sim else 'simulation'
    save_report(options, label, results)


if __name__ == '__main__':
    run_application()

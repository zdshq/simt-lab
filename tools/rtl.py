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
MODULES = {'cache': ('sector_cache', 'test_cache.cpp', ['sector_sram.sv', 'cache_lru.sv', 'sector_cache.sv']),
           'scoreboard': ('lane_scoreboard', 'test_scoreboard.cpp', ['lane_scoreboard.sv']),
           'operand_unit': ('register_operand_unit', 'test_operand_unit.cpp',
                            ['register_bank.sv', 'register_file.sv', 'collector_dispatch.sv', 'collector_payload.sv', 'operand_collector.sv',
                             'integer_lane.sv', 'integer_vector.sv', 'integer_execute.sv', 'writeback_payload.sv', 'writeback_unit.sv', 'register_operand_unit.sv']),
           'integer': ('integer_execute', 'test_integer.cpp', ['integer_lane.sv', 'integer_vector.sv', 'integer_execute.sv']),
           'writeback': ('writeback_unit', 'test_writeback.cpp', ['writeback_payload.sv', 'writeback_unit.sv']),
           'collector': ('operand_collector', 'test_collector.cpp', ['collector_dispatch.sv', 'collector_payload.sv', 'operand_collector.sv']),
           'dispatch': ('collector_dispatch', 'test_dispatch.cpp', ['collector_dispatch.sv']),
           'register_file': ('register_file', 'test_register_file.cpp', ['register_bank.sv', 'register_file.sv']),
           'warp_scoreboard': ('warp_scoreboard', 'test_warp_scoreboard.cpp', ['scoreboard_cell.sv', 'warp_scoreboard.sv']),
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
    includes += f' -DSIMT_RTL_WAYS={options.ways}'
    libraries = ' '.join(str(options.build/name) for name in ('libsimt_core.a', 'libsimt_hardware.a', 'libsimt_storage.a'))
    if 'SIMT_TENSOR_RTL:BOOL=ON' in (options.build/'CMakeCache.txt').read_text():
        libraries += ' '+str(options.build/'libsimt_tensor_circuit.a')
    if 'SIMT_CACHE_RTL:BOOL=ON' in (options.build/'CMakeCache.txt').read_text():
        libraries += ' '+str(options.build/'libsimt_cache_circuit.a')
    if 'SIMT_SCOREBOARD_RTL:BOOL=ON' in (options.build/'CMakeCache.txt').read_text():
        libraries += ' '+str(options.build/'libsimt_scoreboard_circuit.a')
    if 'SIMT_REGISTER_RTL:BOOL=ON' in (options.build/'CMakeCache.txt').read_text():
        libraries += ' '+str(options.build/'libsimt_register_circuit.a')
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
    parameters = [f'-GSETS={options.sets}', f'-GWAYS={options.ways}'] if name == 'cache' else []
    if name in ('register_file', 'operand_unit'): parameters = [f'-GSLOTS={options.rf_slots}']
    if name in ('collector', 'dispatch', 'writeback', 'integer'): parameters = [f'-GENTRIES={options.collector_entries}']
    return parameters+[str(ROOT/'rtl'/file) for file in files]+[str(ROOT/'tests/rtl'/test)]


def simulation(options, name):
    top, _, files = MODULES[name]
    destination = options.out/name
    destination.mkdir(parents=True, exist_ok=True)
    if name == 'fma':
        run([sys.executable, str(ROOT/'tests/rtl/fma_vectors.py'), '--out', str(destination/'reference.txt')],
            destination, 'reference')
    if options.gate_sim: verify_netlist(destination, files, cache_parameters(options, name))
    label, objects = ('gate', 'gate-obj') if options.gate_sim else ('simulation', 'obj')
    flags, libraries = compiler_flags(options)
    if name in ('collector', 'dispatch', 'writeback', 'integer'): flags += f' -DCOLLECTOR_ENTRIES={options.collector_entries}'
    command = [tool('verilator'), '--cc', '--exe', '--build', '-j', str(options.jobs),
               '-Wall', '--unroll-count', '4096', '--top-module', top, '--Mdir', str(destination/objects),
               '-CFLAGS', flags, '-LDFLAGS', libraries]+simulation_sources(options, name, destination)
    if not sanitized(options): command.append('--trace')
    # Old Verilator archives retain removed split objects; rebuild the archive from the current object list.
    # 旧版 Verilator 归档会保留已移除的分片对象，按当前对象列表重新建立归档。
    (destination/objects/('V'+top+'__ALL.a')).unlink(missing_ok=True)
    run(command, destination, label+'-build', environment())
    output = execute_simulation(options, destination, destination/objects/('V'+top), label)
    print(output, end='', flush=True)
    return {'checks': len([line for line in output.splitlines() if line.startswith('PASS ')]),
            'command': command, 'parameters': cache_parameters(options, name),
            'sources': source_hashes([ROOT/'rtl'/file for file in files]),
            'waveforms': [] if sanitized(options) else sorted(path.name for path in destination.glob('*.vcd'))}


def yosys_string(value):
    return '"'+str(value).replace('\\', '\\\\').replace('"', '\\"')+'"'


# Gate tests must use a netlist generated from the current RTL, not a stale successful artifact.
# 门级测试必须使用当前 RTL 生成的网表，不能沿用陈旧的成功产物。
def verify_netlist(destination, files, parameters):
    manifest = destination/'netlist.json'
    if not manifest.is_file(): raise RuntimeError('Run --synth before --gate-sim.')
    record = json.loads(manifest.read_text())
    if record.get('parameters', {}) != parameters:
        raise RuntimeError('Gate geometry differs from requested parameters; rerun --synth.')
    if record['sources'] != source_hashes([ROOT/'rtl'/file for file in files]):
        raise RuntimeError('RTL changed since synthesis; rerun --synth.')
    digest = hashlib.sha256((destination/'gates.v').read_bytes()).hexdigest()
    if digest != record['gate_sha256']: raise RuntimeError('Gate netlist differs from synthesis manifest.')


def cache_parameters(options, name):
    if name in ('register_file', 'operand_unit'): return {'SLOTS': options.rf_slots}
    if name in ('collector', 'dispatch', 'writeback', 'integer'): return {'ENTRIES': options.collector_entries}
    return {'SETS': options.sets, 'WAYS': options.ways} if name == 'cache' else {}


# Retain memory macros for storage accounting before mapping the small configuration to generic gates.
# 先保留存储宏以统计存储容量，再将小配置映射为通用门单元。
def synthesis(options, name):
    top, _, files = MODULES[name]
    destination = options.out/name
    destination.mkdir(parents=True, exist_ok=True)
    lines = ['read_verilog -sv '+' '.join(yosys_string(ROOT/'rtl'/file) for file in files)]
    if name in ('collector', 'dispatch', 'writeback', 'integer'): lines.append(f'chparam -set ENTRIES {options.collector_entries} {top}')
    if name in ('register_file', 'operand_unit'): lines.append(f'chparam -set SLOTS {options.rf_slots} {top}')
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
    result = {'top': top, 'parameters': cache_parameters(options, name),
              'memories': memories, 'inferred_stat': 'inferred-stat.json',
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
    parser.add_argument('--collector-entries', type=int, choices=(2, 4, 8, 16, 32), default=32)
    parser.add_argument('--rf-slots', type=int, choices=(2, 4, 8, 16, 32), default=8)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--synth', action='store_true')
    parser.add_argument('--infer-only', action='store_true')
    parser.add_argument('--gate-sim', action='store_true')
    parser.add_argument('--simcells', type=Path, default=ROOT/'.tools/rtl/yosys-src/techlibs/common/simcells.v')
    options = parser.parse_args()
    if options.module in ('writeback', 'all') and options.collector_entries < 4:
        parser.error('writeback validation requires at least four completion entries')
    options.build, options.out = options.build.resolve(), options.out.resolve()
    options.simcells = options.simcells.resolve()
    if options.sets < 2 or options.sets > 256 or options.sets & (options.sets-1):
        parser.error('--sets must be a power of two in [2, 256]')
    if options.ways != 4 and options.module != 'cache':
        parser.error('--ways 8 requires --module cache')
    if options.infer_only and not options.synth: parser.error('--infer-only requires --synth')
    if options.gate_sim and options.synth: parser.error('--gate-sim and --synth are separate steps')
    return options


def run_application():
    options = parse_options()
    names = MODULES if options.module == 'all' else [options.module]
    action = synthesis if options.synth else simulation
    label = 'synthesis' if options.synth else 'gate' if options.gate_sim else 'simulation'
    # A failed rerun must not leave a previous success manifest masquerading as the current result.
    # 复跑失败时不能留下此前成功清单，让它冒充本次结果。
    (options.out/(label+'.json')).unlink(missing_ok=True)
    results = {name: action(options, name) for name in names}
    save_report(options, label, results)


if __name__ == '__main__':
    run_application()

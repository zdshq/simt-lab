#!/usr/bin/env python3
# Preserve actual host, cache, DMA, and Tensor execution evidence on a persistent timed device.
# 保存持久时序设备上的实际主机、缓存、DMA 和 Tensor 执行证据。
import argparse
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


# Compare downloaded bytes with the workload's independent mathematical reference.
# 将下载字节与工作负载的独立数学参考比较。
def verify(workload, data):
    code = 'f' if workload.kind == 'float' else 'I'
    actual = struct.unpack('<'+code*len(workload.expected), data)
    errors = [abs(value-expected) for value, expected in zip(actual, workload.expected)]
    for value, expected, error in zip(actual, workload.expected, errors):
        limit = workload.atol+workload.rtol*abs(expected) if code == 'f' else 0
        if not math.isfinite(value) or error > limit:
            raise AssertionError(f'{workload.name}: {value} != {expected}')
    return {'verified_values': len(actual), 'max_absolute_error': max(errors, default=0)}


# Each replay contains an upload, the unchanged computation, and a dependent download.
# 每次重放包含上传、保持原算法的计算，以及依赖计算完成的下载。
def make_graph(device, workload, buffer):
    kernel = device.load(workload.program)
    graph = device.graph()
    upload = graph.add(device.op_h2d(buffer, workload.image.data))
    compute = graph.add(kernel.operation(blocks=workload.blocks, threads=workload.threads,
                                         buffers=(buffer,)), (upload,))
    download = graph.add(device.op_d2h(buffer.at(workload.output-buffer.address),
                                      len(workload.expected)*4), (compute,))
    return graph.instantiate(), compute, download


def save_inputs(workload, buffer, destination, backend):
    from simt_lab.runtime import listing
    destination.mkdir(parents=True, exist_ok=True)
    workload.program.save(destination/'kernel.bin')
    workload.image.save(destination/'upload.bin')
    (destination/'kernel.asm').write_text(listing(workload.program))
    save_json(destination/'launch.json', {'base': buffer.address, 'size': buffer.size,
              'blocks': workload.blocks, 'threads': workload.threads, 'timing': True, 'tensor_backend': backend})


def run_workload(device, factory, destination):
    buffer = device.allocate(len(factory().image.data))
    workload = factory(base=buffer.address)
    save_inputs(workload, buffer, destination, device.properties['tensor_backend'])
    executable, compute, download = make_graph(device, workload, buffer)
    records = []
    for replay in range(2):
        before = device.stats()['ticks']
        run = executable.launch()
        data = run.node_result(download)
        record = verify(workload, data)
        kernel = run.node_event(compute).query()
        backends = {item['backend'] for item in kernel['hardware']['tensor']}
        if backends != {device.properties['tensor_backend']}:
            raise AssertionError('unexpected_tensor_backend')
        caches = kernel['hardware']['l1']+[kernel['hardware']['l2']]
        if {item['backend'] for item in caches} != {device.properties['cache_backend']}:
            raise AssertionError('unexpected_cache_backend')
        record.update(replay=replay, device_cycles=device.stats()['ticks']-before,
                      kernel_cycles=kernel['hardware']['cycles'], tensor_backends=sorted(backends),
                      cache_backend=device.properties['cache_backend'])
        (destination/f'output-{replay}.bin').write_bytes(data)
        save_json(destination/f'kernel-{replay}.json', kernel)
        records.append(record)
    buffer.free()
    return {'name': workload.name, 'replays': records}


# Read-only launches isolate persistent L1 reuse from write-induced invalidations.
# 只读启动单独展示持久 L1 复用，避免写入失效的干扰。
def cache_reuse(device):
    from simt_lab import Program, Z
    buffer = device.allocate(4)
    device.h2d(buffer, struct.pack('<I', 123)).wait()
    program = Program().emit('LDC', rd=0, ra=Z, mode=4).emit('LDG', rd=1, ra=0, mode=4).emit('EXIT')
    kernel = device.load(program)
    cold, warm = [kernel.launch((buffer,), threads=1).wait()['hardware'] for _ in range(2)]
    if warm['cycles'] >= cold['cycles'] or warm['dram']['reads'] != cold['dram']['reads']:
        raise AssertionError('persistent cache reuse failed')
    buffer.free()
    return {'cold_cycles': cold['cycles'], 'warm_cycles': warm['cycles'],
            'extra_dram_reads': warm['dram']['reads']-cold['dram']['reads']}


def run_demo(options):
    from simt_lab.host import Device
    from simt_lab.workloads import vector_add, reduction, gemm
    from simt_lab.transformer import tiny_transformer
    options.out.mkdir(parents=True, exist_ok=True)
    with Device(options.build/'simt-device', timing=True, hardware_trace=True,
                tensor_rtl=options.tensor_rtl, cache_rtl=options.cache_rtl, scoreboard_rtl=options.scoreboard_rtl, register_rtl=options.register_rtl) as device:
        summary = {'cache_reuse': cache_reuse(device), 'workloads': []}
        for factory in (vector_add, reduction, gemm, tiny_transformer):
            summary['workloads'].append(run_workload(device, factory, options.out/factory.__name__))
        summary['device'] = device.stats()
        save_json(options.out/'trace.json', device.trace())
    save_json(options.out/'validation.json', summary)
    print(json.dumps(summary, ensure_ascii=False))


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--out', type=Path, default=ROOT/'artifacts'/'hardware-host')
    parser.add_argument('--tensor-rtl', action='store_true')
    parser.add_argument('--cache-rtl', action='store_true')
    parser.add_argument('--scoreboard-rtl', action='store_true')
    parser.add_argument('--register-rtl', action='store_true')
    return parser.parse_args()


def run_application():
    options = parse_options()
    sys.path[:0] = [str(options.build/'generated'), str(ROOT/'python')]
    run_demo(options)


if __name__ == '__main__':
    run_application()

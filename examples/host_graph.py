#!/usr/bin/env python3
# Compare ordinary submissions, retained graph replays, and stream capture on one device.
# 在同一设备上比较普通提交、保留图重放和 stream capture。
import argparse
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]


# Keep binary32 input values exactly representable for an independent equality check.
# 保持 binary32 输入精确可表示，以执行独立相等性检查。
def pack(values):
    return struct.pack('<'+'f'*len(values),*values)


# Parameters A, B, output, and length are read from per-launch constant words.
# 从逐次启动 constant 字读取 A、B、输出和长度参数。
def vector_kernel():
    from simt_lab import Program, Z
    p = Program().emit('S2R',rd=0,imm=1)
    for register in range(1,5): p.emit('LDC',rd=register,ra=Z,imm=(register-1)*4,mode=4)
    p.emit('S2R',rd=5,imm=3).emit('S2R',rd=6,imm=5).emit('IMAD',rd=0,ra=5,rb=6,rc=0)
    p.emit('ISETP',rd=0,ra=0,rb=4,mode=2).emit('MOVI',rd=5,imm=4).emit('IMUL',rd=5,ra=0,rb=5)
    for register in (1,2,3): p.emit('IADD',rd=register,ra=register,rb=5)
    p.emit('LDG',rd=6,ra=1,mode=4,guard=0).emit('LDG',rd=7,ra=2,mode=4,guard=0)
    return p.emit('FADD',rd=8,ra=6,rb=7).emit('STG',ra=3,rb=8,mode=4,guard=0).emit('EXIT')


# Compare the decoded device output with ordinary host arithmetic.
# 将解码后的设备输出与普通宿主算术比较。
def verify(data, a, b):
    actual = struct.unpack('<'+'f'*len(a),data)
    expected = [x+y for x,y in zip(a,b)]
    if list(actual) != expected:
        raise AssertionError('vector result mismatch')
    return len(expected)


# The baseline submits each operation separately to the same stream.
# 基线将每个操作分别提交到同一个 stream。
def ordinary(device, kernel, buffers, a, b):
    first = device.stats()['write']
    device.h2d(buffers[0],pack(a))
    device.h2d(buffers[1],pack(b))
    kernel.launch((*buffers,len(a)),blocks=math.ceil(len(a)/32))
    result = device.d2h(buffers[2],4*len(a)).result()
    verify(result,a,b)
    return device.stats()['write']-first


# A fork/join graph retains the kernel and copies for later single-packet launches.
# fork/join 图保留 kernel 和拷贝操作，供之后通过单包启动。
def build_graph(device, kernel, buffers, a, b):
    graph = device.graph()
    upload_a = graph.add(device.op_h2d(buffers[0],pack(a)))
    upload_b = graph.add(device.op_h2d(buffers[1],pack(b)))
    compute = graph.add(kernel.operation((*buffers,len(a)),blocks=math.ceil(len(a)/32)),(upload_a,upload_b))
    download = graph.add(device.op_d2h(buffers[2],4*len(a)),(compute,))
    return graph.instantiate(),upload_a,download


# Update only the input node, preserving the instantiated topology for three replays.
# 只更新输入节点，为三次重放保留已实例化拓扑。
def replay(device, executable, upload, download, buffers, a, b):
    records = []
    for iteration in range(3):
        values = [value+iteration for value in a]
        executable.update(upload,device.op_h2d(buffers[0],pack(values)))
        first = device.stats()['write']
        run = executable.launch(stream=1)
        count = verify(run.node_result(download),values,b)
        records.append({'iteration':iteration,'verified':count,'queue_packets':device.stats()['write']-first})
    return records


# Cross-stream events are captured as dependencies without executing the captured work.
# 跨 stream 事件被捕获为依赖，不执行正在捕获的工作。
def captured(device, kernel, buffers, a, b):
    before = device.stats()['write']
    with device.capture(streams=(10,11)) as capture:
        device.h2d(buffers[0],pack(a),stream=10)
        ready = device.record_event(stream=10)
        device.wait_event(ready,stream=11)
        device.h2d(buffers[1],pack(b),stream=11)
        compute = kernel.launch((*buffers,len(a)),blocks=math.ceil(len(a)/32),stream=11)
        output = device.d2h(buffers[2],4*len(a),stream=10,wait_for=(compute,))
    if device.stats()['write'] != before:
        raise AssertionError('capture unexpectedly submitted work')
    run = capture.graph.instantiate().launch(stream=2)
    return verify(run.node_result(output.node),a,b)


# Persist trace evidence along with the actual encoded kernel and summary.
# 保存跟踪证据、实际编码 kernel 和摘要。
def run_demo(options):
    from simt_lab.host import Device
    from simt_lab.runtime import listing
    program = vector_kernel()
    options.out.mkdir(parents=True,exist_ok=True)
    program.save(options.out/'kernel.bin')
    (options.out/'kernel.asm').write_text(listing(program))
    with Device(options.build/'simt-device',timing=options.timing,hardware_trace=options.hardware_trace) as device:
        kernel = device.load(program)
        a,b = [(i%17-8)/4 for i in range(65)],[(i%11-5)/8 for i in range(65)]
        buffers = [device.allocate(4*len(a)) for _ in range(3)]
        summary = {'ordinary_packets':ordinary(device,kernel,buffers,a,b)}
        executable,upload,download = build_graph(device,kernel,buffers,a,b)
        summary['replays'] = replay(device,executable,upload,download,buffers,a,b)
        summary['capture_verified'] = captured(device,kernel,buffers,a,b)
        summary['device'] = device.stats()
        (options.out/'trace.json').write_text(json.dumps(device.trace(),indent=2)+'\n')
        for buffer in buffers: buffer.free()
    (options.out/'validation.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,ensure_ascii=False))


def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build',type=Path,default=ROOT/'build')
    parser.add_argument('--out',type=Path,default=ROOT/'artifacts'/'host-graph')
    parser.add_argument('--timing',action='store_true')
    parser.add_argument('--hardware-trace',action='store_true')
    return parser.parse_args()


# Configure generated imports before constructing any encoded operation.
# 构造编码操作之前配置生成代码的导入路径。
def run_application():
    options = parse_options()
    sys.path[:0] = [str(options.build/'generated'),str(ROOT/'python')]
    run_demo(options)


if __name__ == '__main__':
    run_application()

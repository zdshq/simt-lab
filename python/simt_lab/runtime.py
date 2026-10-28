# Save reproducible artifacts and validate the runner's actual global-memory results.
# 保存可复现产物，并验证运行器实际的 global 内存结果。
import json
import math
from pathlib import Path
import struct
import subprocess


# Expose an indexed assembly listing alongside the machine-code binary.
# 在机器码二进制旁提供带索引的汇编清单。
def listing(program):
    lines = []
    for index,(opcode,operands) in enumerate(program.instructions):
        fields = ', '.join(f'{name}={value}' for name,value in operands.items())
        lines.append(f'{index:04d}  {opcode} {fields}')
    return '\n'.join(lines)+'\n'


# Reconstruct little-endian words from the sparse public snapshot format.
# 从公开稀疏快照格式重建小端字。
def output_values(snapshot, address, count, kind):
    values = dict(snapshot)
    data = bytes(values.get(address+i,0) for i in range(count*4))
    return list(struct.unpack('<'+('f' if kind=='float' else 'I')*count,data))


# Floating tolerances cover the documented host-libm and scalar-reference differences.
# 浮点容差覆盖规范中声明的宿主数学库与标量参考差异。
def compare(workload, report):
    actual = output_values(report['global'],workload.output,len(workload.expected),workload.kind)
    errors = [abs(x-y) for x,y in zip(actual,workload.expected)]
    for index,(value,expected,error) in enumerate(zip(actual,workload.expected,errors)):
        allowed = workload.atol+workload.rtol*abs(expected) if workload.kind=='float' else 0
        if not math.isfinite(value) or error > allowed:
            raise AssertionError(f'{workload.name}[{index}]: actual={value}, expected={expected}, error={error}')
    return {'name':workload.name,'verified_values':len(actual),'max_absolute_error':max(errors,default=0),
            'blocks_completed':report['blocks_completed'],'instruction_steps':report['instruction_steps'],
            'op_counts':report['op_counts']}


# Persist exact inputs and invocation arguments before launching the executable.
# 启动可执行文件之前保存精确输入和调用参数。
def save_inputs(workload, runner, destination, policy):
    destination.mkdir(parents=True,exist_ok=True)
    program, image = destination/'kernel.bin', destination/'global.bin'
    workload.program.save(program)
    workload.image.save(image)
    (destination/'kernel.asm').write_text(listing(workload.program))
    command = [str(Path(runner).resolve()),str(program.resolve()),'--global',str(image.resolve()),
               '--sms','2','--blocks',str(workload.blocks),'--threads',str(workload.threads),
               '--policy',policy,'--warp-policy',policy,'--dump-memory']
    (destination/'launch.json').write_text(json.dumps(command,indent=2)+'\n')
    return command


# Execute machine code and preserve both raw output and comparison evidence.
# 执行机器码，并保留原始输出和比较证据。
def run_workload(workload, runner, destination, policy='rr'):
    destination = Path(destination)
    command = save_inputs(workload,runner,destination,policy)
    result = subprocess.run(command,text=True,capture_output=True,timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    (destination/'state.json').write_text(result.stdout)
    report = json.loads(result.stdout)
    summary = compare(workload,report)
    (destination/'validation.json').write_text(json.dumps(summary,indent=2)+'\n')
    return summary

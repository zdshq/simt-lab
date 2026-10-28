# Run encoded kernels through the C++ executable, never through a Python emulator.
# 通过 C++ 可执行文件运行编码后的 kernel，不用 Python 模拟器替代执行。
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from simt_lab import Program


class SimulationCase(unittest.TestCase):
    # Give each execution its own input files and a bounded subprocess lifetime.
    # 为每次执行提供独立输入文件，并限制子进程运行时间。
    def invoke(self, data, options):
        with tempfile.TemporaryDirectory(prefix='simt-test-') as temp:
            path = Path(temp) / 'kernel.bin'
            path.write_bytes(data)
            command = [os.environ['SIMT_RUNNER'], str(path), '--dump-state', *map(str, options)]
            return subprocess.run(command, text=True, capture_output=True, timeout=8)

    # Check the process boundary before inspecting architectural state.
    # 检查进程运行结果后，再读取架构状态。
    def execute(self, program, *options, error=None):
        data = program.binary() if isinstance(program, Program) else program
        result = self.invoke(data, options)
        if error is not None:
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            report = json.loads(result.stderr)
            self.assertIn(error, report['error'])
            return report
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    # Select a logical register across all lanes of one warp.
    # 读取一个 warp 中所有线程的同一逻辑寄存器。
    def registers(self, report, reg, block=0, warp=0):
        return report['ctas'][block]['warps'][warp]['registers'][reg]

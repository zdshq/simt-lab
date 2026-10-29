# Verify explicit backend selection and unsupported-build errors through public process interfaces.
# 通过公开进程接口验证显式后端选择及不支持构建的错误。
import os
from pathlib import Path
import unittest
from support import SimulationCase
from simt_lab import Program
from simt_lab.host import Device


class BackendSelectionTest(SimulationCase):
    def test_rtl_requires_timing(self):
        self.execute(Program().emit('EXIT'), '--tensor-rtl', error='tensor_rtl_requires_timing')
        with self.assertRaisesRegex(ValueError, 'tensor_rtl_requires_timing'):
            Device(tensor_rtl=True)

    def test_explicit_backend_or_unavailable_error(self):
        executable = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        with Device(executable, timing=True) as device:
            available = device.properties['tensor_rtl_available']
            self.assertIsInstance(available, bool)
            self.assertEqual(device.properties['tensor_backend'], 'model')
        program = Program().emit('EXIT')
        if not available:
            self.execute(program, '--timing', '--tensor-rtl', error='tensor_rtl_backend_unavailable')
            return
        report = self.execute(program, '--timing', '--tensor-rtl')['hardware']
        self.assertEqual({sm['backend'] for sm in report['tensor']}, {'rtl'})
        self.assertEqual({sm['array_clocks'] for sm in report['tensor']}, {report['cycles']})


if __name__ == '__main__':
    unittest.main()

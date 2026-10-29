# Selection errors must remain explicit before creating a kernel or claiming RTL execution.
# 在建立 kernel 或宣称 RTL 执行前，选择错误必须明确暴露。
import os
from pathlib import Path
import unittest
from simt_lab import Program
from simt_lab.host import Device
from support import SimulationCase


class CollectorSelection(SimulationCase):
    def test_requires_register_backend(self):
        self.execute(Program().emit('EXIT'), '--timing', '--collector-rtl',
                     error='collector_rtl_requires_register_rtl')
        with self.assertRaisesRegex(ValueError, 'collector_rtl_requires_register_rtl'):
            Device(collector_rtl=True)

    def test_hardware_capacity_and_lifecycle(self):
        executable = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        with Device(executable, timing=True) as device:
            self.assertEqual(device.properties['collector_backend'], 'model')
            if not device.properties['register_rtl_available']:
                self.skipTest('register/collector circuit not built')
        p = Program().emit('MOVI', rd=1, imm=42).emit('IADD', rd=2, ra=1, rb=1).emit('EXIT')
        result = self.execute(p, '--timing', '--register-rtl', '--collector-rtl', '--collectors', 1)
        hardware = result['hardware']
        collectors = hardware['collectors']
        self.assertEqual(self.registers(result, 2), [84]*32)
        self.assertEqual(sum(c['allocated'] for c in collectors), hardware['issued'])
        self.assertEqual(sum(c['released'] for c in collectors), hardware['issued'])
        self.assertTrue(all(c['occupancy'] == 0 and c['limit'] == 1 for c in collectors))
        self.assertEqual(sum(c['reads'] for c in collectors), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)

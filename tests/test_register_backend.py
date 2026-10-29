# Reject unavailable or incompatible physical register geometries before any instruction executes.
# 在执行任何指令前拒绝不可用或不兼容的物理寄存器几何。
import os
from pathlib import Path
import unittest
from support import SimulationCase
from simt_lab import Program, Z
from simt_lab.host import Device


class RegisterSelection(SimulationCase):
    def test_requires_timing_and_available_backend(self):
        self.execute(Program().emit('EXIT'), '--register-rtl', error='register_rtl_requires_timing')
        with self.assertRaisesRegex(ValueError, 'register_rtl_requires_timing'):
            Device(register_rtl=True)
        executable = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        with Device(executable, timing=True) as device:
            self.assertEqual(device.properties['register_backend'], 'model')
            available = device.properties['register_rtl_available']
        if not available:
            self.execute(Program().emit('EXIT'), '--timing', '--register-rtl',
                         error='register_rtl_backend_unavailable')
            return
        for option, value in (('--register-banks', 8), ('--partitions', 3), ('--sm-registers', 65537)):
            self.execute(Program().emit('EXIT'), '--timing', '--register-rtl', option, value,
                         error='register_rtl_geometry_requires')

    # Reusing physical slot zero must clear all registers while preserving retired CTA observations.
    # 复用物理零号槽时必须清除全部寄存器，同时保留已退休 CTA 的观测值。
    def test_physical_slot_reuse_and_actual_port_events(self):
        executable = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        with Device(executable, timing=True) as device:
            if not device.properties['register_rtl_available']:
                self.skipTest('register RTL not built')
        p = Program().emit('IADD', rd=2, ra=1, rb=Z).emit('MOVI', rd=1, imm=99).emit('EXIT')
        result = self.execute(p, '--timing', '--register-rtl', '--sms', 1, '--blocks', 8,
                              '--threads', 32, '--max-warps', 1)
        for cta in result['ctas']:
            registers = cta['warps'][0]['registers']
            self.assertEqual(registers[1], [99]*32)
            self.assertEqual(registers[2], [0]*32)
        files = result['hardware']['register_files']
        self.assertEqual(sum(file['cleared_slots'] for file in files), 8)
        self.assertEqual(sum(file['reads'] for file in files), 8)
        self.assertEqual(sum(file['writes'] for file in files), 16)


if __name__ == '__main__':
    unittest.main(verbosity=2)

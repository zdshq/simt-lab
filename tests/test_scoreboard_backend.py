# Backend selection must be explicit through both CLI and persistent host interfaces.
# CLI 和持久主机接口都必须显式选择后端。
import os
from pathlib import Path
import unittest
from support import SimulationCase
from simt_lab import Program
from simt_lab.host import Device


class ScoreboardSelection(SimulationCase):
    def test_requires_timing(self):
        self.execute(Program().emit('EXIT'), '--scoreboard-rtl', error='scoreboard_rtl_requires_timing')
        with self.assertRaisesRegex(ValueError, 'scoreboard_rtl_requires_timing'):
            Device(scoreboard_rtl=True)
        for timeout in (0, -1, float('inf'), float('nan')):
            with self.assertRaisesRegex(ValueError, 'invalid_device_transport_timeout'):
                Device(request_timeout=timeout)

    def test_build_capability_and_geometry(self):
        runner = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        with Device(runner, timing=True) as device:
            self.assertEqual(device.properties['scoreboard_backend'], 'model')
            available = device.properties['scoreboard_rtl_available']
        if not available:
            self.execute(Program().emit('EXIT'), '--timing', '--scoreboard-rtl',
                         error='scoreboard_rtl_backend_unavailable')
            return
        self.execute(Program().emit('EXIT'), '--timing', '--scoreboard-rtl', '--register-banks', 8,
                     error='scoreboard_rtl_requires_at_most_4_banks')
        report = self.execute(Program().emit('MOVI', rd=3, imm=42).emit('EXIT'), '--timing', '--scoreboard-rtl')
        hardware = report['hardware']
        self.assertEqual(len(hardware['scoreboards']), 1)
        self.assertEqual(hardware['scoreboards'][0]['backend'], 'rtl')
        self.assertEqual(hardware['scoreboards'][0]['clocks'], hardware['cycles'])


if __name__ == '__main__':
    unittest.main(verbosity=2)

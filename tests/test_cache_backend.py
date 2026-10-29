# Verify explicit cache selection and build capability through public interfaces.
# 通过公开接口验证显式缓存选择与构建能力。
import os
from pathlib import Path
import unittest
from support import SimulationCase
from simt_lab import Program, Z
from simt_lab.host import Device


class CacheBackendSelection(SimulationCase):
    def test_cache_rtl_requires_timing(self):
        self.execute(Program().emit('EXIT'), '--cache-rtl', error='cache_rtl_requires_timing')
        with self.assertRaisesRegex(ValueError, 'cache_rtl_requires_timing'):
            Device(cache_rtl=True)

    def test_cache_rtl_is_explicit(self):
        runner = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        with Device(runner, timing=True) as device:
            available = device.properties['cache_rtl_available']
            self.assertEqual(device.properties['cache_backend'], 'model')
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('EXIT')
        if not available:
            self.execute(p, '--timing', '--cache-rtl', error='cache_rtl_backend_unavailable')
            return
        hardware = self.execute(p, '--timing', '--cache-rtl')['hardware']
        for cache in hardware['l1']+[hardware['l2']]:
            self.assertEqual(cache['backend'], 'rtl')
            self.assertEqual(cache['clocks'], hardware['clock_end'])
        self.assertEqual(hardware['l1'][0]['fill_bytes'], 32)
        self.assertEqual(hardware['l2']['fill_bytes'], 32)


if __name__ == '__main__':
    unittest.main(verbosity=2)

# Host graphs preserve the same physical register clock and isolate failed kernel state.
# 主机图保持同一物理寄存器堆时钟，并隔离失败 kernel 的状态。
import unittest
import test_timed_host as timed
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


class CollectorDevice:
    def device(self, **options):
        device = super().device(register_rtl=True, collector_rtl=True, request_timeout=180, **options)
        self.assertEqual(device.properties['collector_backend'], 'rtl')
        return device


class CollectorHostContracts(CollectorDevice, timed.TimedHostContracts):
    pass


class CollectorGraphContracts(CollectorDevice, timed.TimedGraphContracts):
    pass


class CollectorPersistentHardware(CollectorDevice, timed.PersistentHardwareTest):
    pass


class CollectorLifecycle(CollectorDevice, timed.TimedDevice, HostCase):
    def test_queries_do_not_clock_and_failed_kernel_can_restart(self):
        device = self.device()
        program = Program().emit('LDG', rd=1, ra=Z, mode=4).emit('IADD', rd=2, ra=1, rb=1).emit('EXIT')
        module = device.load(program)
        job = module.launch(threads=32, max_steps=1)
        for _ in range(1000):
            device.progress()
            snapshot = job.query()
            if snapshot['hardware'] and snapshot['hardware']['collectors']:
                break
        else:
            self.fail('no live register observed')
        clocks = snapshot['hardware']['collectors']
        self.assertEqual({board['backend'] for board in clocks}, {'rtl'})
        for _ in range(20):
            self.assertEqual(job.query(), snapshot)
        with self.assertRaisesRegex(DeviceError, 'step_limit'):
            job.wait()
        result = module.launch(threads=32, stream=2).wait()['hardware']
        self.assertEqual(result['issued'], 3)
        self.assertEqual(result['retired'], 3)
        self.assertEqual(result['collectors'][0]['clocks'], result['cycles'])


if __name__ == '__main__':
    unittest.main(verbosity=2)

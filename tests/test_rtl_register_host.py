# Host graphs preserve the same physical register clock and isolate failed kernel state.
# 主机图保持同一物理寄存器堆时钟，并隔离失败 kernel 的状态。
import unittest
import test_timed_host as timed
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


class RegisterFileDevice:
    def device(self, **options):
        device = super().device(register_rtl=True, request_timeout=180, **options)
        self.assertEqual(device.properties['register_backend'], 'rtl')
        return device


class RegisterFileHostContracts(RegisterFileDevice, timed.TimedHostContracts):
    pass


class RegisterFileGraphContracts(RegisterFileDevice, timed.TimedGraphContracts):
    pass


class RegisterFilePersistentHardware(RegisterFileDevice, timed.PersistentHardwareTest):
    pass


class RegisterFileLifecycle(RegisterFileDevice, timed.TimedDevice, HostCase):
    def test_queries_do_not_clock_and_failed_kernel_can_restart(self):
        device = self.device()
        program = Program().emit('LDG', rd=1, ra=Z, mode=4).emit('IADD', rd=2, ra=1, rb=1).emit('EXIT')
        module = device.load(program)
        job = module.launch(threads=32, max_steps=1)
        for _ in range(1000):
            device.progress()
            snapshot = job.query()
            if snapshot['hardware'] and snapshot['hardware']['register_files']:
                break
        else:
            self.fail('no live register observed')
        clocks = snapshot['hardware']['register_files']
        self.assertEqual({board['backend'] for board in clocks}, {'rtl'})
        for _ in range(20):
            self.assertEqual(job.query(), snapshot)
        with self.assertRaisesRegex(DeviceError, 'step_limit'):
            job.wait()
        result = module.launch(threads=32, stream=2).wait()['hardware']
        self.assertEqual(result['issued'], 3)
        self.assertEqual(result['retired'], 3)
        self.assertEqual(result['register_files'][0]['clocks'], result['cycles'])


if __name__ == '__main__':
    unittest.main(verbosity=2)

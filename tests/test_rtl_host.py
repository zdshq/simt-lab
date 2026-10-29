# Run persistent host and Graph contracts with one actual RTL array per simulated SM.
# 使用每个模拟 SM 独立的实际 RTL 阵列运行持久主机与 Graph 契约。
import struct
import unittest
import test_timed_host as timed
from host_support import HostCase
from simt_lab import Z
from simt_lab.tensor import MatrixDescriptor
from simt_lab.transport import DeviceError
from tensor_support import tensor_program


class RtlDevice:
    def device(self, **options):
        device = super().device(tensor_rtl=True, **options)
        self.assertEqual(device.properties['tensor_backend'], 'rtl')
        self.assertTrue(device.properties['tensor_rtl_available'])
        return device


class RtlHostContracts(RtlDevice, timed.TimedHostContracts):
    pass


class RtlGraphContracts(RtlDevice, timed.TimedGraphContracts):
    pass


class RtlPersistentHardware(RtlDevice, timed.PersistentHardwareTest):
    pass


# The second warp exhausts a shared instruction budget while the first owns an unfinished MMA.
# 第二个 warp 消耗共享指令预算，此时第一个 warp 持有尚未完成的 MMA。
def interrupted_tensor_kernel():
    descriptor = MatrixDescriptor(0, 4, 4, 256, 4096, 8192, space=0)
    program = tensor_program(descriptor, columns=8)
    program.emit('S2R', rd=2, imm=2).emit('ISETP', rd=1, ra=2, rb=Z)
    program.emit('BRA', imm='worker', guard=9)
    program.emit('MMA', ra=Z, guard=0).emit('EXIT').label('worker')
    program.emit('NOP').emit('BRA', imm='worker')
    return program


class RtlHostExecution(RtlDevice, timed.TimedDevice, HostCase):
    def assert_rtl(self, hardware, stopped=False):
        self.assertEqual({sm['backend'] for sm in hardware['tensor']}, {'rtl'})
        clocks = {sm['array_clocks'] for sm in hardware['tensor']}
        self.assertEqual(len(clocks), 1)
        # Fault recovery freezes private compute state while accepted global requests drain.
        # 故障恢复冻结私有计算状态，同时排空已接受的 global 请求。
        if stopped:
            self.assertLessEqual(clocks.pop(), hardware['cycles'])
        else:
            self.assertEqual(clocks, {hardware['cycles']})

    def launch_dma(self, device, stream=0):
        source, output = device.allocate(16), device.allocate(4)
        device.h2d(source, struct.pack('<4f', 2, 3, 5, 7), stream=stream).wait()
        job = device.load(timed.dma_tensor_kernel()).launch((source, output), threads=1, stream=stream)
        return job, output

    # Status and management queries must preserve both RTL state and the shared device clock.
    # 状态和管理查询必须同时保留 RTL 状态及共享设备时钟。
    def test_query_does_not_clock_active_rtl(self):
        device = self.device()
        job, output = self.launch_dma(device)
        for _ in range(5000):
            device.progress()
            snapshot = job.query()
            hardware = snapshot['hardware']
            if hardware and hardware['tensor'][0]['products']:
                break
        else:
            self.fail('no RTL product event observed')
        self.assertEqual(snapshot['state'], 'running')
        self.assert_rtl(hardware)
        stats, trace = device.stats(), device.trace()
        for _ in range(20):
            self.assertEqual(job.query(), snapshot)
            self.assertEqual(device.stats(), stats)
            self.assertEqual(device.trace(), trace)
        finished = job.wait()['hardware']
        self.assert_rtl(finished)
        self.assertEqual(finished['tensor'][0]['products'], 2)
        self.assertEqual(struct.unpack('<f', device.d2h(output, 4).result()), (31,))

    def test_fault_discards_private_rtl_state_before_next_kernel(self):
        device = self.device()
        job = device.load(interrupted_tensor_kernel()).launch(threads=64, max_steps=150, stream=1)
        with self.assertRaisesRegex(DeviceError, 'step_limit'):
            job.wait()
        hardware = job.query()['hardware']
        self.assert_rtl(hardware, stopped=True)
        products = hardware['tensor'][0]['products']
        self.assertGreater(products, 0)
        self.assertLess(products, 4*4*256)
        actions = {event['action'] for event in hardware['trace']}
        self.assertIn('tensor_tile_start', actions)
        self.assertNotIn('tensor_tile_store', actions)
        next_job, output = self.launch_dma(device, stream=2)
        following = next_job.wait()['hardware']
        self.assert_rtl(following)
        self.assertEqual(following['tensor'][0]['products'], 2)
        self.assertEqual(struct.unpack('<f', device.d2h(output, 4, stream=2).result()), (31,))


if __name__ == '__main__':
    unittest.main(verbosity=2)

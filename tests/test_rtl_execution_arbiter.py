# Exercise shared execution ownership through actual kernels, finite DMA capacity and host recovery.
# 通过实际 kernel、有限 DMA 容量和主机恢复验证共享执行归属。
from collections import Counter, defaultdict
import struct
import unittest
from support import SimulationCase
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


def check_arbiters(test, hardware):
    units = hardware['execution_arbiters']
    test.assertTrue(units)
    for unit in units:
        test.assertEqual((unit['backend'], unit['physical_partitions'], unit['ports_per_partition'], unit['storage_bits']), ('rtl', 8, 7, 778))
        test.assertFalse(unit['tensor_busy'])
        test.assertEqual(unit['accepted'][5], unit['tensor_releases'])
        test.assertEqual(unit['clocks'], hardware['cycles'])
        test.assertLessEqual(unit['active_clocks'], unit['clocks'])
        for pipe in range(7):
            stalls = sum(unit[name][pipe] for name in ('interval_stalls', 'shared_stalls', 'arbitration_stalls'))
            test.assertEqual(unit['requests'][pipe], unit['grants'][pipe]+stalls)
            test.assertEqual(unit['grants'][pipe], unit['accepted'][pipe]+unit['downstream_stalls'][pipe])
        executed = sum(e['sm'] == unit['sm'] and e['action'] == 'execute' for e in hardware['trace'])
        test.assertEqual(sum(unit['accepted']), executed)


class ExecutionArbitration(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        result = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', '--scoreboard-rtl',
                                 '--hardware-trace', '--sms', 1, *options, **kwargs)
        if kwargs.get('error') is None: check_arbiters(self, result['hardware'])
        return result

    def test_private_intervals_on_every_partition(self):
        p = Program()
        pipes = []
        for op, pipe in (('MOVI', 0), ('FADD', 1), ('FEXP2', 2)):
            for reg in range(8):
                operands = {'imm': 0} if op == 'MOVI' else {'ra': Z, 'rb': Z} if op == 'FADD' else {'ra': Z}
                p.emit(op, rd=reg, **operands); pipes.append(pipe)
        p.emit('EXIT')
        for parts in (1, 2, 4, 8):
            result = self.execute(p, '--threads', parts*32, '--partitions', parts, '--collectors', 16,
                                  '--int-interval', 3, '--fp-interval', 5, '--sfu-interval', 7)
            starts = defaultdict(list)
            for e in result['hardware']['trace']:
                if e['action'] == 'execute' and e['pc'] < len(pipes): starts[e['warp'], pipes[e['pc']]].append(e['cycle'])
            for (_, pipe), cycles in starts.items():
                self.assertEqual(len(cycles), 8)
                self.assertTrue(all(b-a >= (3, 5, 7)[pipe] for a, b in zip(cycles, cycles[1:])))
            for warp in range(parts): self.assertEqual([self.registers(result, r, warp=warp)[0] for r in range(8)], [0x3f800000]*8)
            self.assertTrue(all(v > 0 for v in result['hardware']['execution_arbiters'][0]['interval_stalls'][:3]))

    def test_shared_tensor_lease_across_eight_partitions(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z)
        tensor_pcs = {len(p.instructions)}; p.emit('TMEM_ALLOC', rd=1, imm=8, guard=0).emit('BAR_SYNC', rd=0)
        for _ in range(3):
            tensor_pcs.add(len(p.instructions)); p.emit('LDT', rd=3, ra=1, rb=Z, guard=0).emit('BAR_SYNC', rd=0)
        tensor_pcs.add(len(p.instructions)); p.emit('TMEM_FREE', ra=1, imm=8, guard=0).emit('EXIT')
        result = self.execute(p, '--threads', 256, '--partitions', 8)
        busy = None; counts = Counter()
        for e in result['hardware']['trace']:
            if e['pc'] not in tensor_pcs: continue
            if e['action'] == 'execute':
                self.assertIsNone(busy); busy = e['instruction']; counts[e['warp']] += 1
            elif e['action'] == 'retire': self.assertEqual(busy, e['instruction']); busy = None
        self.assertIsNone(busy); self.assertEqual(dict(counts), {warp: 5 for warp in range(8)})
        self.assertEqual(sorted(self.registers(result, 1, warp=w)[0] for w in range(8)), list(range(0, 64, 8)))
        arb = result['hardware']['execution_arbiters'][0]
        self.assertGreater(arb['arbitration_stalls'][5], 0); self.assertGreater(arb['shared_stalls'][5], 0)

    # A one-command DMA pool must still drain while all partitions keep requesting the shared control port.
    # 单命令 DMA 池在全部分区持续请求共享控制端口时仍须排空。
    def test_async_contention_and_dma_completion_priority(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z).emit('S2R', rd=1, imm=2)
        p.emit('MOVI', rd=2, imm=32).emit('IMUL', rd=3, ra=1, rb=2).emit('MB_INIT', ra=3, imm=1, guard=0)
        p.emit('BAR_SYNC', rd=0).emit('MOVI', rd=2, imm=4).emit('IMUL', rd=4, ra=1, rb=2)
        p.emit('MOVI', rd=2, imm=64).emit('IADD', rd=5, ra=4, rb=2)
        p.emit('MOVI', rd=2, imm=2048).emit('IADD', rd=6, ra=4, rb=2)
        p.emit('MB_EXPECT', ra=3, imm=4, guard=0).emit('CP_ASYNC', ra=5, rb=6, rc=3, imm=4, guard=0)
        p.emit('MB_ARRIVE', rd=7, ra=3, guard=0).emit('MB_WAIT', ra=3, rb=Z, guard=0)
        p.emit('BAR_SYNC', rd=0).emit('LDS', rd=8, ra=6, mode=4).emit('EXIT')
        result = self.execute(p, '--threads', 256, '--partitions', 8, '--async-depth', 1, '--async-fragments', 1,
                              '--async-chunk', 4, images={'global': bytes(64)+struct.pack('<8I', *range(100, 108))})
        for warp in range(8): self.assertEqual(self.registers(result, 8, warp=warp), [100+warp]*32)
        arb = result['hardware']['execution_arbiters'][0]
        self.assertEqual(arb['accepted'][6], 40)
        self.assertGreater(arb['arbitration_stalls'][6], 0); self.assertGreater(arb['shared_stalls'][6], 0)
        dma = result['hardware']['async'][0]['dma_engine']
        self.assertEqual((dma['submitted'], dma['released'], dma['commands']), (8, 8, 0))

    def test_predicated_off_async_does_not_claim_busy_control(self):
        p = Program().emit('S2R', rd=0, imm=2).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('S2R', rd=1, imm=0).emit('ISETP', rd=1, ra=1, rb=Z).emit('BRA', imm='idle', guard=8)
        p.emit('MB_INIT', ra=Z, imm=1, guard=1).emit('MB_ARRIVE', rd=2, ra=Z, guard=1).emit('EXIT')
        p.label('idle')
        for _ in range(6): p.emit('MB_INIT', ra=Z, imm=1, guard=0)
        p.emit('EXIT')
        result = self.execute(p, '--threads', 64, '--partitions', 2)
        unit = result['hardware']['execution_arbiters'][0]
        self.assertEqual(unit['accepted'][6], 8)
        self.assertEqual(result['hardware']['async'][0]['barrier_requests'], 2)


class ArbitrationHost(HostCase):
    def test_readonly_active_tensor_lease_and_failed_kernel_recovery(self):
        device = self.device(timing=True, register_rtl=True, collector_rtl=True, hardware_trace=True)
        program = Program().emit('S2R', rd=0, imm=2).emit('ISETP', rd=0, ra=0, rb=Z).emit('BRA', imm='worker', guard=8)
        program.emit('S2R', rd=0, imm=0).emit('ISETP', rd=1, ra=0, rb=Z).emit('TMEM_ALLOC', rd=1, imm=8, guard=1).emit('EXIT')
        program.label('worker').emit('NOP').emit('BRA', imm='worker')
        module = device.load(program); job = module.launch(threads=64, max_steps=40, stream=1)
        for _ in range(1000):
            device.progress(); snapshot = job.query(); hardware = snapshot.get('hardware')
            if hardware and any(u['tensor_busy'] for u in hardware['execution_arbiters']): break
        else: self.fail('active hardware Tensor lease never observed')
        for _ in range(20): self.assertEqual(job.query(), snapshot)
        with self.assertRaisesRegex(DeviceError, 'step_limit'): job.wait()
        check_arbiters(self, module.launch(threads=1, stream=2).wait()['hardware'])


if __name__ == '__main__':
    unittest.main(verbosity=2)

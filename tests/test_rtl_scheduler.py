# Verify hardware scheduling through actual ISA execution and persistent host jobs.
# 通过实际 ISA 执行与持久主机任务验证硬件调度。
from collections import Counter
import struct
import unittest
from support import SimulationCase
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


def check_scheduler(test, hardware):
    units = hardware['warp_schedulers']
    test.assertTrue(units)
    for unit in units:
        test.assertEqual((unit['backend'], unit['physical_slots'], unit['probe_ports'], unit['storage_bits']), ('rtl', 32, 1, 325))
        test.assertEqual((unit['live'], unit['boundary'], unit['memory'], sum(unit['inflight'])), (0, 0, 0, 0))
        test.assertEqual(unit['admitted'], unit['released'])
        test.assertEqual(unit['clocks'], hardware['cycles'])
        test.assertLessEqual(unit['active_clocks'], unit['clocks'])
        test.assertLessEqual(unit['probes'], unit['clocks'])
        test.assertLessEqual(unit['peak_inflight'], 32)
        rejections = sum(unit[name+'_stalls'] for name in ('guard', 'dependency', 'boundary', 'memory', 'count'))
        test.assertEqual(unit['probes'], unit['issued']+rejections)
    test.assertEqual(sum(u['issued'] for u in units), hardware['issued'])
    test.assertEqual(sum(u['retired'] for u in units), hardware['retired'])


class SchedulerExecution(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        result = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', '--scoreboard-rtl',
                                 '--hardware-trace', *options, **kwargs)
        if kwargs.get('error') is None: check_scheduler(self, result['hardware'])
        return result

    def test_all_warp_slots_and_partition_geometries(self):
        p = Program().emit('S2R', rd=0, imm=1).emit('MOVI', rd=1, imm=3).emit('MOVI', rd=3, imm=7)
        p.emit('IMAD', rd=2, ra=0, rb=1, rc=3).emit('EXIT')
        for parts in (1, 2, 4, 8):
            with self.subTest(partitions=parts):
                result = self.execute(p, '--sms', 1, '--threads', 1024, '--partitions', parts)
                for warp in range(32): self.assertEqual(self.registers(result, 2, warp=warp), [(warp*32+lane)*3+7 for lane in range(32)])
                units = result['hardware']['warp_schedulers']
                self.assertEqual([u['admitted'] for u in units], [32//parts]*parts)
                issues = Counter((e['cycle'], e['warp'] % parts) for e in result['hardware']['trace'] if e['action'] == 'issue')
                self.assertEqual(max(issues.values()), 1)

    # A highest-priority warp blocked on memory must yield probe opportunities to other resident warps.
    # 最高优先级 warp 被内存阻塞时，必须将候选检查机会让给其他驻留 warp。
    def test_blocked_priority_warp_does_not_starve_others(self):
        for policy in ('rr', 'min', 'max'):
            slow = 3 if policy == 'max' else 0
            # Keep the blocked load and other warps' work in the same cached instruction line.
            # 将阻塞读取与其他 warp 的工作放在同一已缓存指令行内。
            p = Program().emit('NOP').emit('S2R', rd=0, imm=2).emit('MOVI', rd=3, imm=slow).emit('ISETP', rd=0, ra=0, rb=3)
            load_pc = len(p.instructions)
            p.emit('LDG', rd=1, ra=Z, mode=4, guard=0).emit('IADD', rd=2, ra=1, rb=1, guard=0)
            p.emit('IADD', rd=4, ra=4, rb=Z, guard=8).emit('EXIT')
            result = self.execute(p, '--sms', 1, '--threads', 128, '--partitions', 1, '--warp-policy', policy,
                                  images={'global': struct.pack('<I', 13)})
            trace = result['hardware']['trace']
            start = next(e['cycle'] for e in trace if e['pc'] == load_pc and e['action'] == 'issue')
            end = next(e['cycle'] for e in trace if e['pc'] == load_pc and e['action'] == 'retire')
            self.assertTrue(any(e['warp'] != slow and e['action'] == 'issue' and start < e['cycle'] < end for e in trace))
            self.assertEqual(self.registers(result, 2, warp=slow), [26]*32)
            self.assertGreater(result['hardware']['warp_schedulers'][0]['dependency_stalls'], 0)

    def test_memory_and_boundary_retirement_order(self):
        p = Program().emit('LDG', rd=0, ra=Z, mode=4).emit('MOVI', rd=1, imm=17)
        p.emit('LDG', rd=2, ra=Z, imm=4, mode=4).emit('FENCE').emit('EXIT')
        result = self.execute(p, '--threads', 1, images={'global': struct.pack('<2I', 31, 47)})
        trace = result['hardware']['trace']
        cycle = lambda pc, action: next(e['cycle'] for e in trace if e['pc'] == pc and e['action'] == action)
        self.assertLess(cycle(1, 'issue'), cycle(0, 'retire'))
        self.assertGreaterEqual(cycle(2, 'issue'), cycle(0, 'retire'))
        self.assertGreaterEqual(cycle(3, 'issue'), cycle(2, 'retire'))
        self.assertEqual([self.registers(result, r)[0] for r in range(3)], [31, 17, 47])
        self.assertGreater(result['hardware']['warp_schedulers'][0]['memory_stalls'], 0)

    def test_tail_warps_and_repeated_cta_slot_reuse(self):
        p = Program().emit('S2R', rd=0, imm=1).emit('IADD', rd=1, ra=0, rb=0).emit('BAR_SYNC', rd=0).emit('EXIT')
        result = self.execute(p, '--sms', 1, '--blocks', 7, '--threads', 37, '--max-ctas', 1, '--partitions', 1)
        for cta in range(7):
            self.assertEqual(self.registers(result, 1, block=cta), [lane*2 for lane in range(32)])
            self.assertEqual(self.registers(result, 1, block=cta, warp=1)[:5], [thread*2 for thread in range(32, 37)])
        self.assertEqual(result['hardware']['warp_schedulers'][0]['admitted'], 14)


class SchedulerHost(HostCase):
    def test_readonly_snapshots_and_failed_kernel_recovery(self):
        device = self.device(timing=True, register_rtl=True, collector_rtl=True, scoreboard_rtl=True)
        p = Program().emit('LDG', rd=1, ra=Z, mode=4).emit('MOVI', rd=2, imm=9)
        p.emit('IADD', rd=3, ra=1, rb=2).emit('EXIT')
        module = device.load(p); failed = module.launch(threads=32, max_steps=1, stream=1)
        for _ in range(1000):
            device.progress(); snapshot = failed.query()
            if snapshot.get('hardware') and snapshot['hardware']['warp_schedulers']: break
        else: self.fail('scheduler never became observable')
        for _ in range(20): self.assertEqual(failed.query(), snapshot)
        with self.assertRaisesRegex(DeviceError, 'step_limit'): failed.wait()
        result = module.launch(threads=32, stream=2).wait()['hardware']
        check_scheduler(self, result)
        self.assertEqual((result['issued'], result['retired']), (4, 4))


if __name__ == '__main__':
    unittest.main(verbosity=2)

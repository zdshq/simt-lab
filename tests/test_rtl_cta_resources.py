# Exercise real resource admission and retirement through instructions and asynchronous payloads.
# 通过实际指令和异步载荷验证资源准入及退休。
import unittest
from support import SimulationCase
from simt_lab import Program
from test_timed_async import copy_setup, copy_command, shared_bytes


class CtaResourcesRtl(SimulationCase):
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl',
                                 '--hardware-trace', *options, **kwargs)
        if kwargs.get('error'):
            return report
        resources = report['hardware']['cta_resources']
        self.assertEqual(sum(s['admitted'] for s in resources), report['blocks_completed'])
        for state in resources:
            self.assertEqual(state['backend'], 'rtl')
            self.assertEqual(state['physical_cta_slots'], 64)
            self.assertEqual(state['physical_warps'], 32)
            self.assertEqual(state['admitted'], state['released'])
            self.assertEqual(state['admitted'], state['retiring'])
            self.assertEqual(state['dma_started'], state['dma_finished'])
            self.assertEqual(state['clocks'], report['hardware']['cycles'])
            self.assertLessEqual(state['active_clocks'], state['clocks'])
            for key in ('live', 'cleanup', 'releasing', 'ctas', 'warps', 'registers', 'shared', 'pending'):
                self.assertEqual(state[key], 0, key)
        return report

    def test_partial_warps_and_resource_limits(self):
        p = Program().emit('S2R', rd=0, imm=3).emit('EXIT')
        for option, value in (('--max-ctas', 2), ('--sm-registers', 8192), ('--max-warps', 4)):
            result = self.execute(p, '--threads', 33, '--blocks', 7, '--sms', 1,
                                  '--shared-bytes', 129, option, value)
            resources = result['hardware']['cta_resources'][0]
            self.assertEqual(resources['peak_ctas'], 2)
            self.assertEqual(resources['peak_registers'], 8192)
            self.assertEqual(resources['peak_shared'], 512)
            for block in range(7):
                self.assertEqual(self.registers(result, 0, block), [block]*32)
                self.assertEqual(self.registers(result, 0, block, 1)[0], block)

    def test_one_admission_per_sm_per_clock_and_final_credit_edge(self):
        p = Program().emit('S2R', rd=0, imm=3).emit('EXIT')
        result = self.execute(p, '--threads', 1, '--blocks', 40, '--sms', 2,
                              '--max-ctas', 64, '--shared-bytes', 128)
        events = result['hardware']['trace']
        admissions = [(e['sm'], e['cycle']) for e in events if e['action'] == 'admit']
        self.assertEqual(len(admissions), len(set(admissions)))
        self.assertEqual(len(admissions), 40)
        final_retirement = max(e['cycle'] for e in events if e['action'] == 'cta_retire')
        self.assertGreater(result['hardware']['cycles'], final_retirement)

    def test_exit_retains_credits_until_dma_and_table_cleanup(self):
        p = copy_command(copy_setup(512), 512).emit('EXIT')
        data = bytes(index % 251 for index in range(576))
        result = self.execute(p, '--threads', 1, '--blocks', 4, '--sms', 1,
                              '--max-ctas', 1, '--async-fragments', 1, images={'global': data})
        state = result['hardware']['cta_resources'][0]
        self.assertEqual((state['dma_started'], state['table_cleanups'], state['table_initializations']), (4, 4, 4))
        for block in range(4):
            events = [e for e in result['hardware']['trace'] if e['block'] == block]
            copied = max(e['cycle'] for e in events if e['action'] == 'copy_complete')
            retired = max(e['cycle'] for e in events if e['action'] == 'cta_retire')
            self.assertGreater(retired, copied)
            self.assertEqual(shared_bytes(result, 8192, 512, block), data[64:])

    def test_full_1024_thread_ctas_reuse_all_registers(self):
        result = self.execute(Program().emit('EXIT'), '--threads', 1024, '--blocks', 3, '--sms', 1)
        state = result['hardware']['cta_resources'][0]
        self.assertEqual((state['peak_ctas'], state['peak_registers'], state['peak_shared']), (1, 65536, 65536))


if __name__ == '__main__':
    unittest.main()

# Existing numerical and hazard references now depend on the RTL operand collector.
# 既有数值和冒险参考现在依赖RTL 操作数收集器。
import argparse
import sys
import unittest
import test_timing as timing
import test_timed_memory as memory
import test_timed_async as asynchronous
import test_timed_tensor as tensor
from support import SimulationCase
from simt_lab import Program


class RtlCollector:
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            self.assertTrue(hardware['collectors'])
            self.assertTrue(hardware['writeback_units'])
            self.assertTrue(hardware['integer_units'])
            self.assertTrue(hardware['floating_units'])
            for unit in hardware['floating_units']:
                self.assertEqual(unit['backend'], 'rtl')
                self.assertEqual(unit['pipeline_stages'], 4)
                self.assertEqual(unit['occupancy'], 0)
                self.assertEqual(unit['accepted'], unit['completed'])
                self.assertEqual(unit['clocks'], hardware['cycles'])
            for unit in hardware['integer_units']:
                self.assertEqual(unit['backend'], 'rtl')
                self.assertEqual(unit['entries'], 32)
                self.assertEqual(unit['occupancy'], 0)
                self.assertEqual(unit['accepted'], unit['completed'])
                self.assertEqual(unit['clocks'], hardware['cycles'])
            for field in ('reserved', 'completed', 'retired'):
                self.assertEqual(sum(unit[field] for unit in hardware['writeback_units']), hardware['issued'])
            for unit in hardware['writeback_units']:
                self.assertEqual(unit['backend'], 'rtl')
                self.assertEqual(unit['entries'], 32)
                self.assertEqual(unit['occupancy'], 0)
                self.assertEqual(unit['clocks'], hardware['cycles'])
            for board in hardware['collectors']:
                self.assertEqual(board['backend'], 'rtl')
                self.assertGreater(board['clocks'], 0)
                self.assertLessEqual(board['clocks'], hardware['cycles'])
        return report


class CollectorPipeline(RtlCollector, timing.TimingTest):
    pass


class CollectorControl(RtlCollector, timing.TimedControlTest):
    pass


class CollectorScalar(RtlCollector, timing.TimedScalarTest):
    pass


class CollectorMemory(RtlCollector, timing.TimedMemoryTest):
    pass


class CollectorMemoryContracts(RtlCollector, memory.TimedMemoryContracts):
    pass


class CollectorMemoryPipeline(RtlCollector, memory.MemoryPipelineTest):
    pass


class CollectorAsyncContracts(RtlCollector, asynchronous.TimedAsyncTest):
    pass


class CollectorAsyncPipeline(RtlCollector, asynchronous.DmaPipelineTest):
    pass


class CollectorTensorContracts(RtlCollector, tensor.TimedTensorSemantics):
    pass


class CollectorTensorPipeline(RtlCollector, tensor.TensorPipelineTest):
    pass


class WritebackCapacity(RtlCollector, SimulationCase):
    # Long-latency independent results exhaust physical completion slots before collector capacity.
    # 长延迟独立结果先耗尽物理完成槽，而不是收集器容量。
    def test_completion_reservations_backpressure_issue(self):
        program = Program()
        for reg in range(40):
            program.emit('MOVI', rd=reg, imm=100+reg)
        result = self.execute(program.emit('EXIT'), '--threads', 1, '--int-latency', 100, '--hardware-trace')
        self.assertGreater(result['hardware']['completion_stalls'], 0)
        self.assertEqual([self.registers(result, reg)[0] for reg in range(40)], list(range(100, 140)))
        trace = result['hardware']['trace']
        active, peak = 0, 0
        for event in trace:
            if event['action'] == 'issue':
                active += 1
                peak = max(peak, active)
            elif event['action'] == 'retire':
                active -= 1
            self.assertLessEqual(active, 32)
        self.assertEqual((active, peak), (0, 32))

    def test_integer_hardware_latency_and_dependent_result(self):
        program = Program().emit('MOVI', rd=1, imm=0xffffffff).emit('MOVI', rd=2, imm=3)
        program.emit('IMAD', rd=3, ra=1, rb=2, rc=2).emit('IADD', rd=4, ra=3, rb=2).emit('EXIT')
        for latency in (1, 4, 100):
            result = self.execute(program, '--threads', 1, '--int-latency', latency, '--hardware-trace')
            self.assertEqual(self.registers(result, 3)[0], 0)
            self.assertEqual(self.registers(result, 4)[0], 3)
            self.assertEqual(sum(unit['accepted'] for unit in result['hardware']['integer_units']), 4)
            starts = {event['instruction']: event['cycle'] for event in result['hardware']['trace'] if event['action'] == 'execute'}
            for event in result['hardware']['trace']:
                if event['action'] == 'retire' and event['pc'] < 4:
                    self.assertEqual(event['cycle']-starts[event['instruction']], latency)


# Partition only test scheduling; running this file without a group still discovers every existing case.
# 只划分测试调度；不指定分组运行本文件时仍发现全部既有场景。
def selected_classes(group):
    classes = [name for name, value in vars(sys.modules[__name__]).items()
               if isinstance(value, type) and issubclass(value, unittest.TestCase) and value.__module__ == __name__]
    return [name for name in classes if group == 'all' or test_group(name) == group]


def test_group(name):
    if name.startswith('CollectorTensor'):
        return 'tensor'
    if name.startswith(('CollectorMemory', 'CollectorAsync')):
        return 'memory'
    return 'core'


def run_application():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--group', choices=('all', 'core', 'memory', 'tensor'), default='all')
    options, remaining = parser.parse_known_args()
    unittest.main(defaultTest=selected_classes(options.group), argv=[sys.argv[0], *remaining], verbosity=2)


if __name__ == '__main__':
    run_application()

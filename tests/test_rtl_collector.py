# Existing numerical and hazard references now depend on the RTL operand collector.
# 既有数值和冒险参考现在依赖RTL 操作数收集器。
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


if __name__ == '__main__':
    unittest.main(verbosity=2)

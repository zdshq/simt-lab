# Existing numerical and hazard references now depend on the RTL operand collector.
# 既有数值和冒险参考现在依赖RTL 操作数收集器。
import unittest
import test_timing as timing
import test_timed_memory as memory
import test_timed_async as asynchronous
import test_timed_tensor as tensor


class RtlCollector:
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            self.assertTrue(hardware['collectors'])
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


if __name__ == '__main__':
    unittest.main(verbosity=2)

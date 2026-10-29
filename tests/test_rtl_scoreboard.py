# Existing numerical and hazard references now depend on the clocked RTL scoreboard.
# 既有数值和冒险参考现在依赖时钟驱动的 RTL scoreboard。
import unittest
import test_timing as timing
import test_timed_memory as memory
import test_timed_async as asynchronous
import test_timed_tensor as tensor


class RtlScoreboard:
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--scoreboard-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            self.assertTrue(hardware['scoreboards'])
            for board in hardware['scoreboards']:
                self.assertEqual(board['backend'], 'rtl')
                self.assertGreater(board['clocks'], 0)
                self.assertLessEqual(board['clocks'], hardware['cycles'])
        return report


class ScoreboardPipeline(RtlScoreboard, timing.TimingTest):
    pass


class ScoreboardControl(RtlScoreboard, timing.TimedControlTest):
    pass


class ScoreboardScalar(RtlScoreboard, timing.TimedScalarTest):
    pass


class ScoreboardMemory(RtlScoreboard, timing.TimedMemoryTest):
    pass


class ScoreboardMemoryContracts(RtlScoreboard, memory.TimedMemoryContracts):
    pass


class ScoreboardMemoryPipeline(RtlScoreboard, memory.MemoryPipelineTest):
    pass


class ScoreboardAsyncContracts(RtlScoreboard, asynchronous.TimedAsyncTest):
    pass


class ScoreboardAsyncPipeline(RtlScoreboard, asynchronous.DmaPipelineTest):
    pass


class ScoreboardTensorContracts(RtlScoreboard, tensor.TimedTensorSemantics):
    pass


class ScoreboardTensorPipeline(RtlScoreboard, tensor.TensorPipelineTest):
    pass


if __name__ == '__main__':
    unittest.main(verbosity=2)

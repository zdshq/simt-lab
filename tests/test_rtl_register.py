# Existing numerical and hazard references now depend on the banked RTL register file.
# 既有数值和冒险参考现在依赖分 bank 的 RTL 寄存器堆。
import unittest
import test_timing as timing
import test_timed_memory as memory
import test_timed_async as asynchronous
import test_timed_tensor as tensor


class RtlRegisterFile:
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--register-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            self.assertTrue(hardware['register_files'])
            self.assertTrue(hardware['predicate_files'])
            self.assertEqual(sum(unit['guard_reads'] for unit in hardware['predicate_files']), hardware['issued'])
            for board in hardware['register_files']:
                self.assertEqual(board['backend'], 'rtl')
                self.assertGreater(board['clocks'], 0)
                self.assertLessEqual(board['clocks'], hardware['cycles'])
        return report


class RegisterFilePipeline(RtlRegisterFile, timing.TimingTest):
    pass


class RegisterFileControl(RtlRegisterFile, timing.TimedControlTest):
    pass


class RegisterFileScalar(RtlRegisterFile, timing.TimedScalarTest):
    pass


class RegisterFileMemory(RtlRegisterFile, timing.TimedMemoryTest):
    pass


class RegisterFileMemoryContracts(RtlRegisterFile, memory.TimedMemoryContracts):
    pass


class RegisterFileMemoryPipeline(RtlRegisterFile, memory.MemoryPipelineTest):
    pass


class RegisterFileAsyncContracts(RtlRegisterFile, asynchronous.TimedAsyncTest):
    pass


class RegisterFileAsyncPipeline(RtlRegisterFile, asynchronous.DmaPipelineTest):
    pass


class RegisterFileTensorContracts(RtlRegisterFile, tensor.TimedTensorSemantics):
    pass


class RegisterFileTensorPipeline(RtlRegisterFile, tensor.TensorPipelineTest):
    pass


if __name__ == '__main__':
    unittest.main(verbosity=2)

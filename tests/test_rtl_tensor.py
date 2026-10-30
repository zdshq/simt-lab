# Reuse independent arithmetic references and memory contracts through the physical RTL backend.
# 通过实际 RTL 后端复用独立算术参考和存储契约。
import unittest
import test_tensor as numeric
import test_timed_tensor as timed
from simt_lab import Z, f32
from simt_lab.tensor import MatrixDescriptor, store_words
from tensor_support import tensor_program


class RtlExecution:
    # Instrumented RTL arithmetic needs a longer bound for the full 32x32x256 case.
    # 插桩后的电路算术运行完整 32x32x256 用例，需要更长的时间上限。
    execution_timeout = 60

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--tensor-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            self.assertEqual({sm['backend'] for sm in hardware['tensor']}, {'rtl'})
            self.assertEqual({sm['array_clocks'] for sm in hardware['tensor']}, {hardware['cycles']})
        return report


class RtlTensorSemantics(RtlExecution, numeric.TensorTest):
    pass


class RtlTensorPipeline(RtlExecution, timed.TensorPipelineTest):
    # Physical stages and FIFO depth are fixed; unsupported configurations must fail explicitly.
    # 物理流水级数和 FIFO 深度固定，不支持的配置必须显式失败。
    def test_shared_operand_supply_and_array_latency(self):
        descriptor = MatrixDescriptor(0, 4, 4, 8, 256, 512, space=0)
        program = tensor_program(descriptor, columns=8)
        store_words(program, 256, [f32(2)]*32)
        store_words(program, 512, [f32(3)]*32)
        program.emit('MMA', ra=Z, guard=0).emit('EXIT')
        result = self.timed(program, '--threads', 1)
        self.assertEqual(self.tile(result, descriptor), [f32(48)]*16)
        self.assertEqual(result['hardware']['dram']['data_reads'], 0)
        self.assertEqual(result['hardware']['tensor'][0]['products'], 128)
        for options in (('--tensor-latency', 1), ('--tensor-latency', 32), ('--tensor-queue', 1)):
            self.timed(program, '--threads', 1, *options, error='tensor_rtl_requires_latency_4_and_queue_2')


if __name__ == '__main__':
    unittest.main(verbosity=2)

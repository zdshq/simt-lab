# Existing independent numerical and memory references execute through the actual RTL hierarchy.
# 既有独立数值与存储参考通过实际 RTL 层次执行。
import unittest
import test_timed_memory as memory
import test_timed_async as dma
import test_rtl_tensor as tensor


class RtlCaches:
    # The largest matrix clocks two cache RTL levels as well as instrumented Tensor arithmetic.
    # 最大矩阵同时推进两级缓存 RTL 和经过插桩的 Tensor 算术。
    execution_timeout = 180

    def execute(self, program, *options, **kwargs):
        report = super().execute(program, '--timing', '--cache-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            hardware = report['hardware']
            for cache in hardware['l1']+[hardware['l2']]:
                self.assertEqual(cache['backend'], 'rtl')
                self.assertEqual(cache['clocks'], hardware['clock_end'])
        return report


class CacheMemoryContracts(RtlCaches, memory.TimedMemoryContracts):
    pass


class CacheMemoryPipeline(RtlCaches, memory.MemoryPipelineTest):
    pass


class CacheDmaContracts(RtlCaches, dma.TimedAsyncTest):
    pass


class CacheDmaPipeline(RtlCaches, dma.DmaPipelineTest):
    pass


class CacheTensorSemantics(RtlCaches, tensor.RtlTensorSemantics):
    pass


class CacheTensorPipeline(RtlCaches, tensor.RtlTensorPipeline):
    pass


if __name__ == '__main__':
    unittest.main(verbosity=2)

# End-to-end tests start from encoded kernels and compare host-visible results.
# 端到端测试从编码 kernel 开始，比较宿主可见结果。
import unittest
from support import SimulationCase
from simt_lab.workloads import vector_add, reduction, gemm
from simt_lab.transformer import tiny_transformer
from simt_lab.runtime import compare, output_values


class WorkloadTest(SimulationCase):
    # Check order-independent programs with both PC and warp policies varied.
    # 同时改变 PC 与 warp 策略，检查不依赖顺序的程序。
    def check_workload(self, workload):
        outputs = []
        for policy in ('rr','min','max'):
            report = self.execute(workload.program,'--blocks',workload.blocks,'--threads',workload.threads,
                                  '--policy',policy,'--warp-policy',policy,images={'global':bytes(workload.image.data)})
            self.assertEqual(compare(workload,report)['verified_values'],len(workload.expected))
            outputs.append(output_values(report['global'],workload.output,len(workload.expected),workload.kind))
        self.assertEqual(outputs[0],outputs[1])
        self.assertEqual(outputs[1],outputs[2])
        return report

    # Vector tails cover multiple CTAs and non-multiple-of-warp lengths.
    # 向量尾部覆盖多个 CTA 和非 warp 整数倍长度。
    def test_vector_add(self):
        for length in (1,31,32,33,65,257):
            self.check_workload(vector_add(length))

    # Multiple warp partials must reach the CTA leader through shared memory.
    # 多个 warp 的部分和必须通过 shared 到达 CTA leader。
    def test_shared_reduction(self):
        self.check_workload(reduction())

    # Matrix row tiles execute on both simulated SMs and publish disjoint outputs.
    # 矩阵行 tile 在两个模拟 SM 上执行，并发布互不重叠的输出。
    def test_tiled_gemm(self):
        report = self.check_workload(gemm())
        self.assertEqual({cta['sm_id'] for cta in report['ctas']},{0,1})
        self.assertEqual(report['op_counts']['MMA'],3)

    # Eight MMA commands, scalar normalization, and softmax must all run in the device.
    # 八次 MMA、标量归一化和 softmax 都必须在设备中执行。
    def test_tiny_transformer_block(self):
        report = self.check_workload(tiny_transformer())
        self.assertEqual(report['op_counts']['MMA'],16)
        for operation in ('SHFL','FEXP2','FRSQRT','CP_ASYNC','MB_WAIT','LDT','STG'):
            self.assertGreater(report['op_counts'][operation],0)


if __name__ == '__main__':
    unittest.main(verbosity=2)

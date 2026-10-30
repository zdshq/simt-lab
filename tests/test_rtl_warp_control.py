# Reuse full control-flow references and verify physical token ownership during actual kernel execution.
# 复用完整控制流参考，并在实际 kernel 执行中核对物理令牌归属。
import unittest
import struct
from support import SimulationCase
import test_control
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


# Keep all sixteen tokens live before joining in reverse order.
# 让十六个令牌同时存活，然后按逆序汇合。
def token_program():
    p = Program()
    for token in range(16): p.emit('BSSY', rd=token, imm=f'join{token}')
    for token in reversed(range(16)): p.label(f'join{token}').emit('BSYNC', rd=token)
    return p


class RtlControl:
    execution_timeout = 120

    # Check that every retired control instruction traverses a physical warp instance.
    # 核对每条已退休控制指令都经过物理 warp 实例。
    def execute(self, program, *options, **kwargs):
        result = super().execute(program, '--timing', '--register-rtl', '--collector-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            units = result['hardware']['warp_controls']
            self.assertTrue(units)
            for unit in units:
                self.assertEqual((unit['backend'], unit['tokens'], unit['storage_bits']), ('rtl', 16, 1568))
                self.assertLessEqual(unit['active_clocks'], unit['clocks'])
            expected = sum(result['op_counts'].get(name, 0) for name in ('EXIT', 'BRA', 'BSSY', 'BSYNC', 'BREAK', 'YIELD'))
            self.assertEqual(sum(unit['commands'] for unit in units), expected)
        return result


class ExistingControl(RtlControl, test_control.ControlTest):
    pass


class ControlOwnership(RtlControl, SimulationCase):
    # Exhaust the token bank and force fresh CTAs to reuse previously occupied warp slots.
    # 用满令牌组，并让新 CTA 复用先前占用的 warp 槽。
    def test_all_tokens_and_reused_physical_slots(self):
        p = token_program().emit('EXIT')
        for partitions in (1, 2, 4, 8):
            result = self.execute(p, '--blocks', 9, '--threads', 37, '--max-ctas', 1, '--partitions', partitions)
            units = result['hardware']['warp_controls']
            self.assertEqual(sum(unit['clears'] for unit in units), 18)
            self.assertEqual(sum(unit['woken_lanes'] for unit in units), 9*37*16)

    # The final token and all scheduler policies must wake peers after the other path exits.
    # 使用最后一个令牌时，各调度策略都必须在另一路退出后唤醒同伴。
    def test_last_token_exit_wakeup(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16)
        p.emit('ISETP', rd=0, ra=0, rb=1, mode=2).emit('BSSY', rd=15, imm='join')
        p.emit('BRA', imm='leave', guard=0).emit('BRA', imm='join')
        p.label('leave').emit('YIELD').emit('EXIT')
        p.label('join').emit('BSYNC', rd=15).emit('MOVI', rd=2, imm=73).emit('EXIT')
        for policy in ('rr', 'min', 'max'):
            result = self.execute(p, '--policy', policy)
            self.assertEqual(self.registers(result, 2), [0]*16+[73]*16)
            self.assertEqual(sum(u['woken_lanes'] for u in result['hardware']['warp_controls']), 16)

    # Reject malformed control descriptors through the full ISA execution boundary.
    # 经完整 ISA 执行边界拒绝错误控制描述符。
    def test_invalid_target_and_token(self):
        self.execute(Program().emit('BSSY', rd=63, imm=0), error='reconvergence_id_out_of_range')
        self.execute(Program().emit('BSSY', rd=15, imm='end').label('end').emit('EXIT'), error='invalid_reconvergence_target')
        self.execute(Program().emit('BREAK', rd=15).emit('EXIT'), error='break_unarmed')


# Host Graph replay must reset physical token ownership while preserving actual output writes.
# 主机 Graph 重放必须重置物理令牌归属，同时保留实际输出写入。
class ControlHost(HostCase):
    def device(self):
        return super().device(timing=True, register_rtl=True, collector_rtl=True, request_timeout=120)

    def output_program(self):
        p = token_program().emit('LDC', rd=6, ra=Z, mode=4)
        p.emit('S2R', rd=0, imm=1).emit('S2R', rd=1, imm=3).emit('MOVI', rd=2, imm=37)
        p.emit('IMAD', rd=3, ra=1, rb=2, rc=0).emit('MOVI', rd=4, imm=73)
        p.emit('IADD', rd=4, ra=3, rb=4).emit('MOVI', rd=5, imm=4)
        return p.emit('IMAD', rd=5, ra=3, rb=5, rc=6).emit('STG', ra=5, rb=4, mode=4).emit('EXIT')

    # Tail warps and repeated launches must produce complete memory results after every join.
    # 尾部 warp 和重复启动必须在所有汇合之后生成完整内存结果。
    def test_graph_replays_all_tokens_and_partial_warps(self):
        device = self.device(); output = device.allocate(9*37*4)
        module = device.load(self.output_program()); graph = device.graph()
        clear = graph.add(device.op_h2d(output, bytes(output.size)))
        compute = graph.add(module.operation((output,), blocks=9, threads=37), (clear,))
        download = graph.add(device.op_d2h(output, output.size), (compute,)); executable = graph.instantiate()
        for _ in range(2):
            run = executable.launch()
            self.assertEqual(struct.unpack('<333I', run.node_result(download)), tuple(range(73,406)))
            units = run.node_event(compute).query()['hardware']['warp_controls']
            self.assertEqual({unit['backend'] for unit in units}, {'rtl'})
            self.assertEqual(sum(unit['clears'] for unit in units), 18)
            self.assertEqual(sum(unit['commands'] for unit in units), 18*33)
            self.assertEqual(sum(unit['woken_lanes'] for unit in units), 9*37*16)

    # A failed kernel may leave armed private state, which must not contaminate the next launch.
    # 失败 kernel 可能留下已设置的私有状态，它不能污染下一次启动。
    def test_faulted_token_does_not_leak_into_next_kernel(self):
        device = self.device()
        p = Program().emit('BSSY', rd=15, imm='join').emit('BSYNC', rd=15)
        p.label('join').emit('BSYNC', rd=15).emit('EXIT')
        job = device.load(p).launch()
        with self.assertRaisesRegex(DeviceError, 'bsync_wrong_pc'): job.wait()
        self.assertEqual(sum(u['commands'] for u in job.query()['hardware']['warp_controls']), 1)
        result = device.load(token_program().emit('EXIT')).launch(stream=2).wait()
        self.assertEqual(sum(u['woken_lanes'] for u in result['hardware']['warp_controls']), 32*16)


if __name__ == '__main__':
    unittest.main(verbosity=2)

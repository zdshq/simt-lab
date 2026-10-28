# Check independent PCs and explicit reconvergence under three selection policies.
# 在三种选择策略下检查独立 PC 和显式重聚。
import unittest
from support import SimulationCase
from simt_lab import Program, Z

FULL = (1 << 32) - 1
POLICIES = ('rr', 'min', 'max')


# Split lanes into two halves using P0.
# 使用 P0 将线程分为两半。
def split_program():
    return (Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=16)
            .emit('ISETP', rd=0, ra=0, rb=1, mode=2))


# Construct a nested diamond with different values on all three paths.
# 构造嵌套菱形分支，让三条路径产生不同值。
def nested_diamond():
    p = split_program().emit('BSSY', rd=0, imm='outer').emit('BRA', imm='lower', guard=0)
    p.emit('MOVI', rd=2, imm=30).emit('BRA', imm='outer')
    p.label('lower').emit('BSSY', rd=1, imm='inner').emit('MOVI', rd=1, imm=8)
    p.emit('ISETP', rd=1, ra=0, rb=1, mode=2).emit('BRA', imm='lowest', guard=1)
    p.emit('MOVI', rd=2, imm=20).emit('BRA', imm='inner')
    p.label('lowest').emit('MOVI', rd=2, imm=10)
    p.label('inner').emit('BSYNC', rd=1)
    p.label('outer').emit('BSYNC', rd=0).emit('ACTIVEMASK', rd=3).emit('EXIT')
    return p


# Each lane leaves the loop after its own lane ID iterations.
# 每个线程在执行与自身 lane ID 相同的迭代次数后离开循环。
def counted_loop():
    p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=1)
    p.label('head').emit('BSSY', rd=0, imm='join')
    p.emit('ISETP', rd=0, ra=2, rb=0, mode=2).emit('BREAK', rd=0, guard=8)
    p.emit('BRA', imm='done', guard=8).emit('IADD', rd=2, ra=2, rb=1)
    p.label('join').emit('BSYNC', rd=0).emit('BRA', imm='head')
    return p.label('done').emit('EXIT')


class ControlTest(SimulationCase):
    # Compare architectural state rather than schedule-dependent issue counts.
    # 比较架构状态，而非依赖调度顺序的发射计数。
    def test_nested_reconvergence(self):
        states = []
        for policy in POLICIES:
            result = self.execute(nested_diamond(), '--policy', policy)
            self.assertEqual(self.registers(result, 2), [10]*8 + [20]*8 + [30]*16)
            self.assertEqual(self.registers(result, 3), [FULL]*32)
            states.append(result['ctas'])
        self.assertEqual(states[0], states[1])
        self.assertEqual(states[1], states[2])

    # BREAK changes membership and permits reuse of the same loop token.
    # BREAK 改变参与集合，并允许复用同一个循环令牌。
    def test_divergent_loop_and_token_reuse(self):
        for policy in POLICIES:
            result = self.execute(counted_loop(), '--policy', policy)
            self.assertEqual(self.registers(result, 2), list(range(32)))

    # EXIT removes membership even when the other path is already waiting.
    # 即使另一条路径已经等待，EXIT 也应移除参与关系。
    def test_exit_releases_waiting_join(self):
        p = split_program().emit('BSSY', rd=0, imm='join')
        p.emit('BRA', imm='leave', guard=0).emit('BRA', imm='join')
        p.label('leave').emit('YIELD').emit('EXIT')
        p.label('join').emit('BSYNC', rd=0).emit('MOVI', rd=2, imm=19).emit('EXIT')
        for policy in POLICIES:
            result = self.execute(p, '--policy', policy)
            self.assertEqual(self.registers(result, 2), [0]*16 + [19]*16)

    # A dissolved token is distinct from a token that was never initialized.
    # 已解除令牌与从未初始化的令牌有区别。
    def test_all_members_break_and_yield_alone(self):
        p = Program().emit('BSSY', rd=0, imm='join').emit('BREAK', rd=0)
        p.emit('YIELD').label('join').emit('BSYNC', rd=0).emit('EXIT')
        self.execute(p)
        self.execute(Program().emit('BSYNC', rd=0).emit('EXIT'), error='bsync_unarmed')

    # Membership masks select subgroups, and only members receive the result.
    # 成员掩码选择子组，只有成员接收结果。
    def test_elect_vote_and_subgroup_shuffle(self):
        p = split_program().emit('ELECT', rd=1, imm=0x00FF0000)
        for mode in range(4):
            p.emit('VOTE', rd=2+mode, ra=0, imm=FULL, mode=mode)
        p.emit('VOTE', rd=6, ra=7, imm=0xFF, mode=2)
        p.emit('MOVI', rd=7, imm=3).emit('SHFL', rd=8, ra=0, rb=7, imm=0xFF)
        result = self.execute(p.emit('EXIT'))
        self.assertEqual(result['ctas'][0]['warps'][0]['predicates'][1], 1 << 16)
        for reg, value in {2: 0xFFFF, 3: 1, 4: 0, 5: 0}.items():
            self.assertEqual(self.registers(result, reg), [value]*32)
        self.assertEqual(self.registers(result, 6), [1]*8+[0]*24)
        self.assertEqual(self.registers(result, 8), [3]*8+[0]*24)

    # In-place shuffles must not depend on the host's lane traversal order.
    # 原地 SHFL 不能依赖宿主遍历线程的顺序。
    def test_shuffle_reads_old_registers(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('MOVI', rd=1, imm=31)
        p.emit('ISUB', rd=1, ra=1, rb=0).emit('SHFL', rd=0, ra=0, rb=1, imm=FULL)
        self.assertEqual(self.registers(self.execute(p.emit('EXIT')), 0), list(reversed(range(32))))

    # Collectives reject missing members and undefined source lanes.
    # 集合操作拒绝缺席成员和未定义的源线程。
    def test_invalid_collective_participation(self):
        self.execute(Program().emit('ELECT', rd=0, imm=0), error='empty_collective_members')
        self.execute(split_program().emit('VOTE', rd=2, ra=7, imm=FULL, guard=0),
                     error='membermask_not_subset')
        p = Program().emit('MOVI', rd=1, imm=32).emit('SHFL', rd=2, ra=0, rb=1, imm=FULL)
        self.execute(p, error='shuffle_source_not_member')
        self.execute(Program().emit('ELECT', rd=0, imm=FULL), '--threads', 17,
                     error='membermask_not_subset')

    # Token misuse fails at its originating instruction with contextual errors.
    # 令牌误用在发起指令处失败，并提供上下文。
    def test_invalid_control_operations(self):
        cases = [(Program().emit('BRA', imm=999), 'branch_target_out_of_range'),
                 (Program().emit('BREAK', rd=0), 'break_unarmed'),
                 (Program().emit('BSSY', rd=16, imm=0), 'reconvergence_id_out_of_range'),
                 (Program().emit('BSSY', rd=0, imm=0), 'invalid_reconvergence_target')]
        for program, error in cases:
            self.execute(program, error=error)
        p = Program().emit('BSSY', rd=0, imm=2).emit('BSSY', rd=0, imm=2).emit('BSYNC', rd=0)
        self.execute(p, error='live_barrier_overwrite')
        p = Program().emit('BSSY', rd=0, imm=2).emit('BSYNC', rd=0).emit('BSYNC', rd=0)
        self.execute(p, error='bsync_wrong_pc')

    # Two disjoint arrival sets cannot complete two full-warp tokens.
    # 两个互斥到达集合无法完成两个要求整个 warp 参与的令牌。
    def test_deadlock_and_unbounded_loop(self):
        p = split_program().emit('BSSY', rd=0, imm='a').emit('BSSY', rd=1, imm='b')
        p.emit('BRA', imm='b', guard=0)
        p.label('a').emit('BSYNC', rd=0).emit('EXIT')
        p.label('b').emit('BSYNC', rd=1).emit('EXIT')
        self.execute(p, error='deadlock_no_runnable_group')
        self.execute(Program().emit('BRA', imm=0), '--max-steps', 20, error='step_limit_exceeded')


if __name__ == '__main__':
    unittest.main(verbosity=2)

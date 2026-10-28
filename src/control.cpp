#include "simt/control.hpp"
#include <stdexcept>

namespace simt {
namespace {
// Validate the warp-local token identifier before touching its state.
// 访问状态之前检查 warp 内的重聚令牌编号。
Reconvergence& token(Warp& warp, uint32_t id) {
    if (id >= warp.reconvergence.size()) throw std::runtime_error("reconvergence_id_out_of_range");
    return warp.reconvergence[id];
}

// Wake only lanes waiting for this particular dynamic token.
// 仅唤醒等待当前动态令牌的线程。
void release_token(Warp& warp, uint32_t id) {
    auto& barrier = token(warp, id);
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (warp.state[lane] != LaneState::reconvergence || warp.blocked_token[lane] != id) continue;
        warp.pc[lane] = barrier.target_pc + 1;
        warp.state[lane] = LaneState::active;
    }
    barrier.state = TokenState::unarmed;
    barrier.arrived = 0;
    barrier.participants = 0;
}

// EXIT and BREAK may make a waiting token complete without another arrival.
// EXIT 和 BREAK 可能在没有新线程到达时使等待令牌完成。
void release_ready_tokens(Warp& warp) {
    for (uint32_t id = 0; id < warp.reconvergence.size(); ++id) {
        auto& barrier = warp.reconvergence[id];
        if (barrier.state != TokenState::armed) continue;
        if (!barrier.participants) barrier.state = TokenState::dissolved;
        else if (barrier.arrived == barrier.participants) release_token(warp, id);
    }
}

// Each token records the actual issuing group rather than the whole warp.
// 每个令牌记录实际发起线程组，而不是整个 warp。
void arm_token(const ExecutionContext& context, Group group, const Module& module) {
    const auto& i = context.instruction;
    auto& barrier = token(context.after, i.rd);
    if (i.guard != 7) throw std::runtime_error("predicated_reconvergence_unsupported");
    if (barrier.state == TokenState::armed) throw std::runtime_error("live_barrier_overwrite");
    if (i.imm >= module.code.size() || module.code[i.imm].op != Op::BSYNC || module.code[i.imm].rd != i.rd)
        throw std::runtime_error("invalid_reconvergence_target");
    barrier = {TokenState::armed, group.mask, 0, i.imm};
}

// Stop arrived lanes until every remaining participant reaches the same join.
// 阻塞已到达线程，直到剩余参与者全部到达同一汇合点。
void arrive_token(const ExecutionContext& context, Group group) {
    const auto& i = context.instruction;
    auto& barrier = token(context.after, i.rd);
    if (i.guard != 7) throw std::runtime_error("predicated_reconvergence_unsupported");
    if (barrier.state == TokenState::unarmed) throw std::runtime_error("bsync_unarmed");
    if (barrier.target_pc != group.pc) throw std::runtime_error("bsync_wrong_pc");
    if (barrier.state == TokenState::dissolved) return;
    if (group.mask & ~barrier.participants) throw std::runtime_error("barrier_participation_mismatch");
    barrier.arrived |= group.mask;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(group.mask & lane_bit(lane))) continue;
        context.after.state[lane] = LaneState::reconvergence;
        context.after.pc[lane] = group.pc;
        context.after.blocked_token[lane] = i.rd;
    }
    release_ready_tokens(context.after);
}

// A break removes only its named token; EXIT removes every live membership.
// BREAK 只移除指定令牌的参与关系；EXIT 移除全部有效参与关系。
void remove_members(Warp& warp, uint32_t id, uint32_t mask) {
    auto& barrier = token(warp, id);
    barrier.participants &= ~mask;
    barrier.arrived &= ~mask;
}

// Retiring lanes can never be awaited by a reconvergence token again.
// 重聚令牌不能继续等待已经退出的线程。
void exit_lanes(Warp& warp, uint32_t mask) {
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (mask & lane_bit(lane)) warp.state[lane] = LaneState::exited;
    for (uint32_t id = 0; id < warp.reconvergence.size(); ++id) remove_members(warp, id, mask);
    release_ready_tokens(warp);
}

// Reject breaks that do not refer to a live or explicitly dissolved token.
// 拒绝不指向有效或已明确解除令牌的 BREAK。
void break_lanes(const ExecutionContext& context, uint32_t mask) {
    if (!mask) return;
    auto& barrier = token(context.after, context.instruction.rd);
    if (barrier.state == TokenState::unarmed) throw std::runtime_error("break_unarmed");
    if (barrier.state == TokenState::armed && (mask & ~barrier.participants))
        throw std::runtime_error("barrier_participation_mismatch");
    remove_members(context.after, context.instruction.rd, mask);
    release_ready_tokens(context.after);
}

// Branch targets are instruction indices and apply only to guard-true lanes.
// 分支目标是指令索引，并且仅影响谓词为真的线程。
void branch_lanes(const ExecutionContext& context, uint32_t mask, const Module& module) {
    if (mask && context.instruction.imm >= module.code.size()) throw std::runtime_error("branch_target_out_of_range");
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (mask & lane_bit(lane)) context.after.pc[lane] = context.instruction.imm;
}

// Yield is a scheduling hint with no memory-ordering effect.
// YIELD 是调度提示，不提供内存排序效果。
void yield_lanes(Warp& warp, uint32_t mask) {
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (mask & lane_bit(lane)) warp.state[lane] = LaneState::yielded;
}
}

// A yielded lane waits at most one selection opportunity within its warp.
// YIELD 线程在自己的 warp 内最多让出一次选择机会。
bool resume_yielded(Warp& warp) {
    bool changed = false;
    for (auto& state : warp.state)
        if (state == LaneState::yielded) { state = LaneState::active; changed = true; }
    return changed;
}

// Execute control operations as group transitions with explicit token ownership.
// 将控制操作作为线程组状态变化执行，并明确令牌归属。
bool execute_control(const ExecutionContext& context, Group group, uint32_t mask, const Module& module) {
    switch (context.instruction.op) {
    case Op::BRA: branch_lanes(context, mask, module); return true;
    case Op::BSSY: arm_token(context, group, module); return true;
    case Op::BSYNC: arrive_token(context, group); return true;
    case Op::BREAK: break_lanes(context, mask); return true;
    case Op::EXIT: exit_lanes(context.after, mask); return true;
    case Op::YIELD: yield_lanes(context.after, mask); return true;
    default: return false;
    }
}
}

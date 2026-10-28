#include "simt/memsync.hpp"
#include <stdexcept>

namespace simt {
namespace {
// Initialize one generation, keeping explicit counts distinct from block-wide mode.
// 初始化一个阶段，区分显式计数和整个线程块模式。
CtaBarrier& prepare_barrier(const ExecutionContext& context) {
    const auto& i = context.instruction;
    if (i.rd >= context.cta.barriers.size()) throw std::runtime_error("cta_barrier_id_out_of_range");
    if (i.imm > context.config.threads) throw std::runtime_error("invalid_barrier_count");
    auto& barrier = context.cta.barriers[i.rd];
    if (barrier.active && barrier.count != i.imm) throw std::runtime_error("barrier_count_mismatch");
    if (!barrier.active) { barrier.active = true; barrier.count = i.imm; barrier.arrived.reset(); }
    return barrier;
}

// Arrival counts individual threads even when the same warp takes different paths.
// 即使同一 warp 走不同路径，到达也逐线程计数。
void arrive(const ExecutionContext& context, uint32_t mask) {
    if (!mask) return;
    auto& barrier = prepare_barrier(context);
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(mask & lane_bit(lane))) continue;
        const auto thread = context.warp_id * lanes + lane;
        if (barrier.arrived[thread]) throw std::runtime_error("duplicate_barrier_arrival");
        barrier.arrived.set(thread);
        if (context.instruction.op != Op::BAR_SYNC) continue;
        context.after.state[lane] = LaneState::cta_barrier;
        context.after.blocked_token[lane] = context.instruction.rd;
    }
}

// Exited threads are excluded only if they had not already arrived this generation.
// 只有本阶段尚未到达的退出线程才从动态期望集合移除。
uint32_t expected_count(const Cta& cta, const CtaBarrier& barrier) {
    if (barrier.count) return barrier.count;
    uint32_t count = 0;
    for (uint32_t wi = 0; wi < cta.warps.size(); ++wi)
        for (uint32_t lane = 0; lane < lanes; ++lane)
            if (cta.warps[wi].state[lane] != LaneState::exited || barrier.arrived[wi * lanes + lane]) ++count;
    return count;
}

// Release waiting threads at their own continuation PCs, then advance the generation.
// 在各线程自己的后续 PC 处释放等待线程，再推进阶段。
void release(Cta& cta, uint32_t id) {
    for (auto& warp : cta.warps)
        for (uint32_t lane = 0; lane < lanes; ++lane)
            if (warp.state[lane] == LaneState::cta_barrier && warp.blocked_token[lane] == id)
                warp.state[lane] = LaneState::active;
    auto& barrier = cta.barriers[id];
    barrier.active = false;
    barrier.arrived.reset();
    ++barrier.generation;
}
}

// Run after each committed instruction so EXIT can also complete dynamic barriers.
// 每条指令提交后检查，让 EXIT 也能完成动态屏障。
void release_cta_barriers(Cta& cta) {
    for (uint32_t id = 0; id < cta.barriers.size(); ++id) {
        const auto& barrier = cta.barriers[id];
        if (!barrier.active) continue;
        const auto expected = expected_count(cta, barrier);
        if (barrier.arrived.count() > expected) throw std::runtime_error("barrier_arrival_overflow");
        if (barrier.arrived.count() == expected) release(cta, id);
    }
}

// Arrival-only and arrive-and-wait share accounting but have different blocking effects.
// 仅到达与到达等待共享计数规则，但阻塞效果不同。
bool execute_barrier(const ExecutionContext& context, uint32_t mask) {
    if (context.instruction.op != Op::BAR_SYNC && context.instruction.op != Op::BAR_ARRIVE) return false;
    arrive(context, mask);
    return true;
}
}

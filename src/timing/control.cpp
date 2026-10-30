#include "simt/timing.hpp"
#include "simt/control.hpp"

namespace simt {
static_assert(static_cast<uint32_t>(Op::EXIT) == 24 && static_cast<uint32_t>(Op::BRA) == 25 &&
    static_cast<uint32_t>(Op::BSSY) == 26 && static_cast<uint32_t>(Op::BSYNC) == 27 &&
    static_cast<uint32_t>(Op::BREAK) == 28 && static_cast<uint32_t>(Op::YIELD) == 29);
static_assert(static_cast<uint32_t>(LaneState::active) == 0 && static_cast<uint32_t>(LaneState::exited) == 5 &&
    static_cast<uint32_t>(TokenState::unarmed) == 0 && static_cast<uint32_t>(TokenState::armed) == 1 &&
    static_cast<uint32_t>(TokenState::dissolved) == 2);
static_assert(static_cast<uint32_t>(LaneState::reconvergence) == 1 && static_cast<uint32_t>(LaneState::yielded) == 2 &&
    static_cast<uint32_t>(LaneState::cta_barrier) == 3 && static_cast<uint32_t>(LaneState::async_barrier) == 4);
static_assert(static_cast<uint32_t>(Policy::round_robin) == 0 && static_cast<uint32_t>(Policy::min_pc) == 1 &&
    static_cast<uint32_t>(Policy::max_pc) == 2);

namespace {
// Architectural arrays are observations of physical state, never PC/control operands for the RTL frontend.
// 架构数组是物理状态的观察快照，绝不作为 RTL 前端的 PC 或控制操作数。
void observe(Warp& warp, const hw::FrontendState& value) {
    warp.pc = value.control.lanes.pc; warp.blocked_token = value.control.lanes.blocked;
    warp.waiting_phase = value.phase; warp.valid_mask = value.valid_mask; warp.last_pc = value.last_pc;
    for (uint32_t lane = 0; lane < 32; ++lane) warp.state[lane] = static_cast<LaneState>(value.control.lanes.state[lane]);
    for (uint32_t id = 0; id < 16; ++id) {
        const auto& token = value.control.tokens[id];
        warp.reconvergence[id] = {static_cast<TokenState>(token.state),token.members,token.arrivals,token.target};
    }
}
}
hw::WarpFrontend* TimingMachine::frontend(uint32_t cta, uint32_t warp) const {
    return sms_[machine_.ctas_[cta].sm_id]->controls[warps_[cta][warp].slot].get();
}
void TimingMachine::observe_frontend(uint32_t cta, uint32_t warp) {
    if (auto* unit = frontend(cta,warp)) observe(machine_.ctas_[cta].warps[warp],unit->read());
}

// Unfinished CTA/async engines write only changed wait metadata through the ordered external ports.
// 尚未硬件化的 CTA/异步引擎仅通过有序外部端口写入变化的等待元数据。
void TimingMachine::publish_waiters(Cta& cta) {
    for (uint32_t wi = 0; wi < cta.warps.size(); ++wi) {
        auto* unit = frontend(cta.block_id,wi);
        if (!unit) continue;
        const auto& warp = cta.warps[wi]; hw::FrontendWait waits;
        waits.blocked = warp.blocked_token; waits.phase = warp.waiting_phase;
        for (uint32_t lane = 0; lane < 32; ++lane) waits.state[lane] = static_cast<uint32_t>(warp.state[lane]);
        unit->external(waits); observe_frontend(cta.block_id,wi);
    }
}

// Select from stored PCs and states, retaining the functional path only when the frontend is disabled.
// 从存储的 PC 和状态选择；仅在禁用硬件前端时保留功能参考路径。
std::optional<Group> TimingMachine::select_warp_group(uint32_t cta, uint32_t wi) {
    auto& warp = machine_.ctas_[cta].warps[wi];
    if (auto* unit = frontend(cta,wi)) {
        const auto group = unit->select(static_cast<uint32_t>(machine_.config_.policy));
        observe_frontend(cta,wi);
        if (group) return Group{group->pc,group->mask};
        return {};
    }
    auto group = machine_.select_group(warp);
    if (!group && resume_yielded(warp)) group = machine_.select_group(warp);
    return group;
}
void TimingMachine::issue_frontend(const Inflight& entry) {
    if (auto* unit = frontend(entry.cta,entry.warp)) {
        unit->issue({entry.group.pc,entry.group.mask},!entry.footprint.boundary);
        observe_frontend(entry.cta,entry.warp); return;
    }
    auto& warp = machine_.ctas_[entry.cta].warps[entry.warp]; resume_yielded(warp);
    if (!entry.footprint.boundary)
        for (uint32_t lane = 0; lane < lanes; ++lane)
            if (entry.group.mask & lane_bit(lane)) warp.pc[lane] = entry.group.pc+1;
    warp.last_pc = entry.group.pc;
}
void TimingMachine::advance_warp_pc(const Inflight& entry) {
    if (auto* unit = frontend(entry.cta,entry.warp)) {
        unit->advance({entry.group.pc,entry.group.mask}); observe_frontend(entry.cta,entry.warp); return;
    }
    auto& warp = machine_.ctas_[entry.cta].warps[entry.warp];
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (entry.group.mask & lane_bit(lane)) warp.pc[lane] = entry.group.pc+1;
}

// Execute carries descriptors; lane operands and tokens come from the connected hardware storage.
// execute 仅传递描述符，lane 操作数和令牌来自直连硬件存储。
bool TimingMachine::commit_warp_control(const Inflight& entry, Warp& next) {
    auto* unit = frontend(entry.cta,entry.warp);
    const auto& i = entry.instruction;
    if (!unit || i.op < Op::EXIT || i.op > Op::YIELD) return false;
    const auto& code = machine_.module_.code;
    const bool target_sync = i.imm < code.size() && code[i.imm].op == Op::BSYNC && code[i.imm].rd == i.rd;
    const hw::ControlCommand command{static_cast<uint32_t>(i.op),i.rd,i.guard,entry.group.pc,entry.group.mask,
        entry.mask,i.imm,static_cast<uint32_t>(code.size()),target_sync};
    observe(next,unit->execute(command));
    return true;
}
}

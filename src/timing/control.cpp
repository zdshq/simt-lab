#include "simt/timing.hpp"

namespace simt {
static_assert(static_cast<uint32_t>(Op::EXIT) == 24 && static_cast<uint32_t>(Op::BRA) == 25 &&
    static_cast<uint32_t>(Op::BSSY) == 26 && static_cast<uint32_t>(Op::BSYNC) == 27 &&
    static_cast<uint32_t>(Op::BREAK) == 28 && static_cast<uint32_t>(Op::YIELD) == 29);
static_assert(static_cast<uint32_t>(LaneState::active) == 0 && static_cast<uint32_t>(LaneState::exited) == 5 &&
    static_cast<uint32_t>(TokenState::unarmed) == 0 && static_cast<uint32_t>(TokenState::armed) == 1 &&
    static_cast<uint32_t>(TokenState::dissolved) == 2);

// Lane state is still a host ingress; token snapshots never feed back into the hardware engine.
// lane 状态仍是宿主输入；令牌快照绝不反馈给硬件引擎。
bool TimingMachine::commit_warp_control(const Inflight& entry, Warp& next) {
    auto* unit = sms_[entry.sm]->controls[warps_[entry.cta][entry.warp].slot].get();
    const auto& i = entry.instruction;
    if (!unit || i.op < Op::EXIT || i.op > Op::YIELD) return false;
    const auto& code = machine_.module_.code;
    const bool target_sync = i.imm < code.size() && code[i.imm].op == Op::BSYNC && code[i.imm].rd == i.rd;
    const hw::ControlCommand command{static_cast<uint32_t>(i.op),i.rd,i.guard,entry.group.pc,entry.group.mask,
        entry.mask,i.imm,static_cast<uint32_t>(code.size()),target_sync};
    hw::ControlLanes lanes; lanes.pc = next.pc; lanes.blocked = next.blocked_token;
    for (uint32_t lane = 0; lane < 32; ++lane) lanes.state[lane] = static_cast<uint32_t>(next.state[lane]);
    const auto result = unit->execute(command,lanes);
    next.pc = result.lanes.pc; next.blocked_token = result.lanes.blocked;
    for (uint32_t lane = 0; lane < 32; ++lane) next.state[lane] = static_cast<LaneState>(result.lanes.state[lane]);
    for (uint32_t id = 0; id < 16; ++id) {
        const auto& token = result.tokens[id];
        next.reconvergence[id] = {static_cast<TokenState>(token.state),token.members,token.arrivals,token.target};
    }
    return true;
}
}

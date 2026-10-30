#include "simt/warp_frontend.hpp"
#include "Vsimt_warp_frontend.h"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
// Access packed three-bit state fields without corrupting adjacent lanes across word boundaries.
// 访问打包的三位状态字段，跨字边界时不破坏相邻 lane。
template<class Words> uint32_t state_at(const Words& words, uint32_t lane) {
    const auto shift = lane*3; uint64_t bits = words[shift/32];
    if (shift%32 > 29) bits |= uint64_t{words[shift/32+1]}<<32;
    return (bits>>(shift%32))&7;
}
template<class Words> void put_state(Words& words, uint32_t lane, uint32_t value) {
    const auto shift = lane*3;
    words[shift/32] = (words[shift/32] & ~(7u<<(shift%32))) | (value<<(shift%32));
    if (shift%32 > 29) {
        const auto remaining = 32-shift%32;
        words[shift/32+1] = (words[shift/32+1] & ~(7u>>remaining)) | (value>>remaining);
    }
}

class RtlWarpFrontend final : public WarpFrontend {
public:
    RtlWarpFrontend() { clear_pins(); dut_.clk = 0; dut_.reset = 1; edge(); dut_.reset = 0; dut_.eval(); }
    ~RtlWarpFrontend() override { dut_.final(); }
    void initialize(uint32_t mask) override;
    FrontendState read() const override;
    FrontendState execute(const ControlCommand& command) override;
    std::optional<FrontendGroup> select(uint32_t policy) override;
    void issue(FrontendGroup group, bool advance) override;
    void advance(FrontendGroup group) override;
    void external(const FrontendWait& waits) override;
    void cta_barrier(uint32_t block_mask, uint32_t barrier, uint32_t release_mask) override;
    void tick() override;
    const ControlStats& stats() const override { return control_stats_; }
    const FrontendStats& frontend_stats() const override { return stats_; }
private:
    Vsimt_warp_frontend dut_;
    ControlStats control_stats_;
    FrontendStats stats_;
    void edge() { dut_.clk = 1; dut_.eval(); dut_.clk = 0; dut_.eval(); }
    void clear_pins();
};

// Pins hold only the current cycle's transactions; persistent state lives exclusively in the DUT.
// 引脚只保存当前拍事务，持久状态全部位于 DUT 中。
void RtlWarpFrontend::clear_pins() {
    dut_.initialize = 0; dut_.command_valid = 0; dut_.retire_advance = 0;
    dut_.resume_idle = 0; dut_.issue_valid = 0;
    dut_.write_state_mask = 0; dut_.write_blocked_mask = 0; dut_.write_phase_mask = 0;
    dut_.cta_block_mask = 0; dut_.cta_release_mask = 0;
}
void RtlWarpFrontend::initialize(uint32_t mask) {
    if (dut_.clock_enable) throw std::runtime_error("frontend_initialize_phase");
    dut_.initial_mask = mask; dut_.initialize = 1; dut_.eval();
}

// Observations include same-cycle forwarding but never supply PC or tokens back to the hardware.
// 观察值包含同拍旁路，但绝不将 PC 或令牌作为输入回送硬件。
FrontendState RtlWarpFrontend::read() const {
    FrontendState result;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        result.control.lanes.pc[lane] = dut_.observed_pc[lane];
        result.control.lanes.state[lane] = state_at(dut_.observed_state,lane);
        result.control.lanes.blocked[lane] = dut_.observed_blocked[lane];
        result.phase[lane] = dut_.observed_phase[lane];
    }
    for (uint32_t id = 0; id < 16; ++id)
        result.control.tokens[id] = {(dut_.token_states>>(id*2))&3u,dut_.token_members[id],dut_.token_arrivals[id],dut_.token_targets[id]};
    result.valid_mask = dut_.observed_valid_mask;
    if (dut_.observed_last_valid) result.last_pc = dut_.observed_last_pc;
    return result;
}

// One boundary transaction precedes external post-writes and the next issue in each model cycle.
// 每个模型拍内，一个边界事务先于外部后写入及下一次发射。
FrontendState RtlWarpFrontend::execute(const ControlCommand& c) {
    if (dut_.initialize || dut_.command_valid || dut_.retire_advance || dut_.issue_valid || dut_.resume_idle)
        throw std::runtime_error("frontend_control_phase");
    if (c.opcode > 255 || c.token >= 64 || c.guard >= 16) throw std::out_of_range("control_descriptor");
    dut_.opcode = c.opcode; dut_.token = c.token; dut_.guard = c.guard;
    dut_.group_pc = c.pc; dut_.group_mask = c.group; dut_.mask = c.mask;
    dut_.target_pc = c.target; dut_.code_size = c.code_size; dut_.target_is_sync = c.target_is_sync;
    dut_.command_valid = 1; dut_.eval();
    if (dut_.accept) return read();
    const auto error = dut_.error; dut_.command_valid = 0; dut_.eval();
    static constexpr std::array messages{"control_rejected", "unsupported_control_opcode", "reconvergence_id_out_of_range",
        "predicated_reconvergence_unsupported", "live_barrier_overwrite", "invalid_reconvergence_target", "bsync_unarmed",
        "bsync_wrong_pc", "barrier_participation_mismatch", "break_unarmed", "branch_target_out_of_range"};
    throw std::runtime_error(error < messages.size() ? messages[error] : "unknown_control_error");
}

// Probing an idle warp resumes yielded lanes even when later dependency checks stall its issue.
// 探测空闲 warp 时恢复让出的 lane，即使之后的依赖检查使发射停顿。
std::optional<FrontendGroup> RtlWarpFrontend::select(uint32_t policy) {
    if (policy > 2) throw std::out_of_range("frontend_policy");
    if (dut_.issue_valid) throw std::runtime_error("frontend_select_after_issue");
    dut_.policy = policy; dut_.eval();
    if (!dut_.group_valid) return {};
    if (dut_.needs_resume) { dut_.resume_idle = 1; dut_.eval(); }
    return FrontendGroup{dut_.selected_pc,dut_.selected_mask};
}
void RtlWarpFrontend::issue(FrontendGroup group, bool advance_pc) {
    if (dut_.issue_valid) throw std::runtime_error("frontend_duplicate_issue");
    if (!dut_.group_valid || group.pc != dut_.selected_pc || group.mask != dut_.selected_mask)
        throw std::runtime_error("frontend_issue_group_mismatch");
    dut_.issue_pc = group.pc; dut_.issue_mask = group.mask;
    dut_.issue_advance = advance_pc; dut_.issue_valid = 1; dut_.eval();
}
void RtlWarpFrontend::advance(FrontendGroup group) {
    if (dut_.command_valid || dut_.retire_advance || dut_.issue_valid || dut_.resume_idle)
        throw std::runtime_error("frontend_advance_phase");
    dut_.retire_pc = group.pc; dut_.retire_mask = group.mask; dut_.retire_advance = 1; dut_.eval();
}

// Two ordered write ports accept only external wait metadata; there is no host PC-write port.
// 两个有序写端口仅接收外部等待元数据，不提供宿主 PC 写端口。
void RtlWarpFrontend::external(const FrontendWait& waits) {
    if (dut_.issue_valid || dut_.resume_idle) throw std::runtime_error("frontend_external_phase");
    for (auto state : waits.state) if (state > 5) throw std::out_of_range("control_lane_state");
    const auto current = read(); const uint32_t offset = dut_.command_valid || dut_.retire_advance ? 32 : 0;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        const auto index = offset+lane; const auto bit = uint64_t{1}<<index;
        if (waits.state[lane] != current.control.lanes.state[lane]) {
            dut_.write_state_mask |= bit; put_state(dut_.write_state,index,waits.state[lane]);
        }
        if (waits.blocked[lane] != current.control.lanes.blocked[lane]) {
            dut_.write_blocked_mask |= bit; dut_.write_blocked[index] = waits.blocked[lane];
        }
        if (waits.phase[lane] != current.phase[lane]) { dut_.write_phase_mask |= bit; dut_.write_phase[index] = waits.phase[lane]; }
    }
    dut_.eval();
}

// Route accepted table signals into lane storage; an entire CTA shares one release vector per cycle.
// 将已接受表事务的信号送入 lane 存储，整个 CTA 每拍共享一个释放向量。
void RtlWarpFrontend::cta_barrier(uint32_t block_mask, uint32_t barrier, uint32_t release_mask) {
    if (dut_.issue_valid || dut_.resume_idle || dut_.cta_block_mask || dut_.cta_release_mask)
        throw std::runtime_error("frontend_cta_phase");
    if ((block_mask && barrier>=16) || release_mask>=65536) throw std::out_of_range("frontend_cta_descriptor");
    dut_.cta_block_mask = block_mask; dut_.cta_block_id = barrier&15;
    dut_.cta_release_mask = release_mask; dut_.eval();
}

// Sample accepted events once, commit one edge, then remove all forwarding transactions.
// 只统计一次已接受事件，提交一个边沿，然后撤销全部旁路事务。
void RtlWarpFrontend::tick() {
    ++control_stats_.clocks; ++stats_.clocks;
    control_stats_.active_clocks += bool(dut_.control_clock_enable);
    control_stats_.commands += bool(dut_.accept); control_stats_.clears += bool(dut_.initialize);
    control_stats_.woken_lanes += std::popcount(uint32_t{dut_.wake_mask});
    stats_.issued += bool(dut_.issue_valid); stats_.advances += bool(dut_.retire_advance);
    stats_.external_lanes += std::popcount(uint64_t{dut_.write_state_mask|dut_.write_blocked_mask|dut_.write_phase_mask});
    stats_.resumed_lanes += std::popcount(uint32_t{dut_.resumed_mask});
    stats_.cta_blocked += std::popcount(uint32_t{dut_.cta_block_mask});
    stats_.cta_woken += std::popcount(uint32_t{dut_.cta_wake_mask});
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear_pins(); dut_.eval();
}
}
std::unique_ptr<WarpFrontend> make_warp_frontend() { return std::make_unique<RtlWarpFrontend>(); }
}

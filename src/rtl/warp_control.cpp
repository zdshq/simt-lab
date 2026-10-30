#include "simt/warp_control.hpp"
#include "Vsimt_warp_control.h"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlWarpControl final : public WarpControl {
public:
    RtlWarpControl() { dut_.clk = 0; dut_.command_valid = 0; dut_.clear = 0; dut_.reset = 1; edge(); dut_.reset = 0; dut_.eval(); }
    ~RtlWarpControl() override { dut_.final(); }
    void clear() override {
        if (dut_.command_valid) throw std::runtime_error("control_clear_after_command");
        dut_.clear = 1; dut_.eval();
    }
    ControlResult execute(const ControlCommand& c, const ControlLanes& lanes) override;
    void tick() override;
    const ControlStats& stats() const override { return stats_; }
private:
    Vsimt_warp_control dut_;
    ControlStats stats_;
    void edge() { dut_.clk = 1; dut_.eval(); dut_.clk = 0; dut_.eval(); }
    void drive(const ControlCommand& c, const ControlLanes& lanes);
    ControlResult read() const;
};

// Pack lane states across Verilator's 32-bit words without advancing the source clock.
// 将 lane 状态打包到 Verilator 的 32 位字边界，不推进源时钟。
void RtlWarpControl::drive(const ControlCommand& c, const ControlLanes& lanes) {
    dut_.opcode = c.opcode; dut_.token = c.token; dut_.guard = c.guard;
    dut_.group_pc = c.pc; dut_.group_mask = c.group; dut_.mask = c.mask;
    dut_.target_pc = c.target; dut_.code_size = c.code_size; dut_.target_is_sync = c.target_is_sync;
    for (uint32_t word = 0; word < 3; ++word) dut_.lane_state[word] = 0;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        dut_.lane_pc[lane] = lanes.pc[lane]; dut_.lane_blocked[lane] = lanes.blocked[lane];
        const auto shift = lane*3;
        dut_.lane_state[shift/32] |= lanes.state[lane] << (shift%32);
        if (shift%32 > 29) dut_.lane_state[shift/32+1] |= lanes.state[lane] >> (32-shift%32);
    }
    dut_.command_valid = 1; dut_.eval();
}
// Read combinational lane outputs and token observations; no shadow token state is retained.
// 读取组合 lane 输出和令牌观察值，不保留令牌影子状态。
ControlResult RtlWarpControl::read() const {
    ControlResult result;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        const auto shift = lane*3; uint64_t packed = dut_.next_state[shift/32];
        if (shift%32 > 29) packed |= uint64_t{dut_.next_state[shift/32+1]}<<32;
        result.lanes.pc[lane] = dut_.next_pc[lane]; result.lanes.blocked[lane] = dut_.next_blocked[lane];
        result.lanes.state[lane] = (packed>>(shift%32))&7;
    }
    for (uint32_t id = 0; id < 16; ++id)
        result.tokens[id] = {(dut_.token_states>>(id*2))&3u,dut_.token_members[id],dut_.token_arrivals[id],dut_.token_targets[id]};
    return result;
}

// One pending transaction per physical warp; errors withdraw pins without changing stored tokens.
// 每个物理 warp 只有一个待提交事务；错误撤销引脚，不改变已存令牌。
ControlResult RtlWarpControl::execute(const ControlCommand& c, const ControlLanes& lanes) {
    if (dut_.command_valid || dut_.clear) throw std::runtime_error("control_command_phase");
    if (c.opcode > 255 || c.token >= 64 || c.guard >= 16) throw std::out_of_range("control_descriptor");
    for (auto state : lanes.state) if (state > 5) throw std::out_of_range("control_lane_state");
    drive(c,lanes);
    if (dut_.accept) return read();
    const auto error = dut_.error; dut_.command_valid = 0; dut_.eval();
    static constexpr std::array messages{"control_rejected", "unsupported_control_opcode", "reconvergence_id_out_of_range",
        "predicated_reconvergence_unsupported", "live_barrier_overwrite", "invalid_reconvergence_target", "bsync_unarmed",
        "bsync_wrong_pc", "barrier_participation_mismatch", "break_unarmed", "branch_target_out_of_range"};
    throw std::runtime_error(error < messages.size() ? messages[error] : "unknown_control_error");
}
// Count the stable transaction before its edge, then withdraw command pins to avoid duplicate commits.
// 在边沿前统计稳定事务，随后撤销命令引脚以避免重复提交。
void RtlWarpControl::tick() {
    ++stats_.clocks; stats_.commands += bool(dut_.accept); stats_.clears += bool(dut_.clear);
    stats_.woken_lanes += std::popcount(uint32_t{dut_.wake_mask});
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    if (dut_.command_valid || dut_.clear) { dut_.command_valid = 0; dut_.clear = 0; dut_.eval(); }
}
}
std::unique_ptr<WarpControl> make_warp_control() { return std::make_unique<RtlWarpControl>(); }
}

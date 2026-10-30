#pragma once
#include "simt/collective_execution.hpp"
#include <array>
#include <stdexcept>

namespace simt::hw::detail {
template<class Circuit, uint32_t Slots> class RtlCollective final : public CollectiveExecution {
public:
    explicit RtlCollective(Circuit& circuit) : dut_(circuit) { dut_.collective_launch = 0; dut_.eval(); }
    bool launch(const CollectiveCommand& command) override;
    uint32_t occupancy() const override { return dut_.collective_output_valid; }
    const CollectiveStats& collective_stats() const override { return stats_; }
    void sample() {
        ++stats_.clocks; stats_.active_clocks += bool(dut_.collective_clock_enable);
        stats_.accepted += bool(dut_.collective_accept);
        stats_.vote_reads += dut_.collective_accept && dut_.collective_opcode == 31;
        stats_.completed += bool(dut_.collective_output_valid && (dut_.wb_complete_accept & 16));
        if (dut_.collective_output_valid && (dut_.wb_complete_error & 16))
            throw std::runtime_error("collective_completion_protocol_error");
    }
    void clear() { if (dut_.collective_launch) { dut_.collective_launch = 0; dut_.eval(); } }
private:
    Circuit& dut_;
    CollectiveStats stats_;
};

// Fault pins are checked before changing ownership or advancing the circuit clock.
// 在改变归属或推进电路时钟之前检查错误引脚。
template<class C, uint32_t S> bool RtlCollective<C,S>::launch(const CollectiveCommand& c) {
    if (dut_.collective_launch || !dut_.dispatch_enable || dut_.allocate_valid)
        throw std::runtime_error("collective_launch_phase");
    if (c.collector >= 32 || c.completion >= 32 || c.opcode > 255 || c.mode > 255 || c.ra >= 64 || c.rb >= 64 || c.warp >= S)
        throw std::out_of_range("collective_descriptor");
    dut_.collective_collector = c.collector; dut_.collective_entry = c.completion;
    dut_.collective_opcode = c.opcode; dut_.collective_mode = c.mode;
    dut_.collective_ra = c.ra; dut_.collective_rb = c.rb; dut_.collective_warp = c.warp;
    dut_.collective_mask = c.mask; dut_.collective_members = c.members; dut_.collective_group = c.group;
    dut_.collective_launch = 1; dut_.eval();
    const auto error = dut_.collective_error;
    if (dut_.collective_accept) return true;
    clear();
    static constexpr std::array messages{"", "unsupported_collective_opcode", "empty_collective_members", "membermask_not_subset",
        "predicate_out_of_range", "invalid_vote_mode", "shuffle_source_not_member", "collective_source_missing", "collective_collector_not_selected"};
    if (error) throw std::runtime_error(error < messages.size() ? messages[error] : "unknown_collective_error");
    return false;
}
}

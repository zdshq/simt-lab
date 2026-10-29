#pragma once
#include "simt/integer_execution.hpp"
#include <array>
#include <stdexcept>

namespace simt::hw::detail {
template<class Circuit> class RtlInteger final : public IntegerExecution {
public:
    explicit RtlInteger(Circuit& circuit) : dut_(circuit) { dut_.integer_launch = 0; dut_.eval(); }
    bool launch(const IntegerCommand& command) override;
    uint32_t occupancy() const override { return dut_.integer_occupancy; }
    const IntegerStats& integer_stats() const override { return stats_; }
    void sample() {
        ++stats_.clocks; stats_.active_clocks += bool(dut_.integer_clock_enable);
        stats_.accepted += bool(dut_.integer_accept);
        stats_.completed += bool(dut_.integer_output_valid && (dut_.wb_complete_accept & 1));
        if (dut_.integer_output_valid && (dut_.wb_complete_error & 1)) throw std::runtime_error("integer_completion_rejected");
    }
    void clear() { if (dut_.integer_launch) { dut_.integer_launch = 0; dut_.eval(); } }
private:
    Circuit& dut_;
    IntegerStats stats_;
};

// A failed command is withdrawn before the shared clock; it cannot consume a collector or a result slot.
// 失败命令在共用时钟到来前撤销，不能消耗收集器或结果槽。
template<class C> bool RtlInteger<C>::launch(const IntegerCommand& c) {
    if (dut_.integer_launch || !dut_.dispatch_enable || dut_.allocate_valid)
        throw std::runtime_error("integer_launch_phase");
    if (c.collector >= 32 || c.completion >= 32 || c.opcode > 255 || c.mode > 255 || c.ra >= 64 || c.rb >= 64 || c.rc >= 64)
        throw std::out_of_range("integer_descriptor");
    dut_.integer_collector = c.collector; dut_.integer_entry = c.completion;
    dut_.integer_opcode = c.opcode; dut_.integer_mode = c.mode;
    dut_.integer_ra = c.ra; dut_.integer_rb = c.rb; dut_.integer_rc = c.rc;
    dut_.integer_mask = c.mask; dut_.integer_immediate = c.immediate; dut_.integer_latency = c.latency;
    dut_.integer_warp = c.warp; dut_.integer_block = c.block; dut_.integer_sm = c.sm;
    dut_.integer_threads = c.threads; dut_.integer_blocks = c.blocks;
    dut_.integer_launch = 1; dut_.eval();
    const auto error = dut_.integer_error;
    if (dut_.integer_accept) return true;
    clear();
    static constexpr std::array messages{"", "unsupported_integer_opcode", "invalid_integer_comparison",
        "unknown_special_register", "invalid_cvta_mode", "cvta_address_out_of_range", "integer_source_missing",
        "invalid_integer_latency", "integer_collector_not_selected"};
    if (error) throw std::runtime_error(error < messages.size() ? messages[error] : "unknown_integer_error");
    return false;
}
}

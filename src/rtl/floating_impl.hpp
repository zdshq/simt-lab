#pragma once
#include "simt/floating_execution.hpp"
#include <array>
#include <stdexcept>

namespace simt::hw::detail {
template<class Circuit> class RtlFloating final : public FloatingExecution {
public:
    explicit RtlFloating(Circuit& circuit) : dut_(circuit) { dut_.floating_launch = 0; dut_.eval(); }
    bool launch(const FloatingCommand& command) override;
    uint32_t occupancy() const override { return dut_.floating_occupancy; }
    const FloatingStats& floating_stats() const override { return stats_; }
    void sample() {
        ++stats_.clocks; stats_.active_clocks += bool(dut_.floating_clock_enable);
        stats_.accepted += bool(dut_.floating_accept);
        stats_.completed += bool(dut_.floating_output_valid && (dut_.wb_complete_accept & 2));
        if (dut_.floating_protocol_error || (dut_.floating_output_valid && (dut_.wb_complete_error & 2)))
            throw std::runtime_error("floating_completion_protocol_error");
    }
    void clear() { if (dut_.floating_launch) { dut_.floating_launch = 0; dut_.eval(); } }
private:
    Circuit& dut_;
    FloatingStats stats_;
};

// Validation observes circuit error pins before any clock or collector ownership change.
// 检查在任何时钟或收集器归属变化前读取电路错误引脚。
template<class C> bool RtlFloating<C>::launch(const FloatingCommand& c) {
    if (dut_.floating_launch || !dut_.dispatch_enable || dut_.allocate_valid)
        throw std::runtime_error("floating_launch_phase");
    if (c.collector >= 32 || c.completion >= 32 || c.opcode > 255 || c.mode > 255 || c.ra >= 64 || c.rb >= 64 || c.rc >= 64)
        throw std::out_of_range("floating_descriptor");
    dut_.floating_collector = c.collector; dut_.floating_entry = c.completion;
    dut_.floating_opcode = c.opcode; dut_.floating_mode = c.mode;
    dut_.floating_ra = c.ra; dut_.floating_rb = c.rb; dut_.floating_rc = c.rc;
    dut_.floating_mask = c.mask; dut_.floating_latency = c.latency;
    dut_.floating_launch = 1; dut_.eval();
    const auto error = dut_.floating_error;
    if (dut_.floating_accept) return true;
    clear();
    static constexpr std::array messages{"", "unsupported_floating_opcode", "invalid_float_comparison",
        "float_to_integer_out_of_range", "floating_source_missing", "floating_rtl_requires_latency_at_least_4",
        "floating_collector_not_selected"};
    if (error) throw std::runtime_error(error < messages.size() ? messages[error] : "unknown_floating_error");
    return false;
}
}

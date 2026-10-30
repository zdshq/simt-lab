#pragma once
#include "simt/writeback.hpp"
#include <bit>
#include <stdexcept>

namespace simt::hw::detail {
template<class Circuit, uint32_t Slots> class RtlWriteback final : public Writeback {
public:
    explicit RtlWriteback(Circuit& circuit) : dut_(circuit) {
        dut_.wb_reserve_valid = 0; dut_.wb_complete_valid = 0; dut_.wb_acknowledge = 0; dut_.eval();
    }
    bool available() const override { return !dut_.wb_reserve_valid && dut_.wb_reserve_ready; }
    uint32_t reserve(const WritebackDestination& destination) override;
    bool complete(uint32_t entry, uint32_t pipe, const RegisterVector& values) override;
    void arbitrate() override { sealed_ = true; }
    bool completed(uint32_t entry) const override {
        if (entry >= 32) throw std::out_of_range("writeback_entry");
        return (dut_.wb_completed_entries & (1u << entry)) != 0;
    }
    bool selected(uint32_t entry) const override;
    RegisterVector take(uint32_t entry) override;
    uint32_t occupancy() const override { return dut_.wb_occupancy; }
    const WritebackStats& writeback_stats() const override { return stats_; }
    void sample();
    void clear();
    void close_retirement() { sealed_ = true; retirement_closed_ = true; }
private:
    Circuit& dut_;
    WritebackStats stats_;
    bool sealed_ = false, retirement_closed_ = false;
};

template<class C, uint32_t S> uint32_t RtlWriteback<C,S>::reserve(const WritebackDestination& d) {
    if (d.warp >= S || d.pipe >= 7 || d.kind > 2 || d.reg >= 64 || (d.kind == 2 && d.reg >= 7))
        throw std::out_of_range("writeback_descriptor");
    if (!available()) throw std::runtime_error("writeback_reservation_capacity");
    dut_.wb_reserve_warp = d.warp; dut_.wb_reserve_pipe = d.pipe; dut_.wb_reserve_kind = d.kind;
    dut_.wb_reserve_register = d.reg; dut_.wb_reserve_mask = d.mask; dut_.wb_reserve_valid = 1; dut_.eval();
    if (!dut_.wb_reserve_ready) throw std::runtime_error("writeback_reservation_rejected");
    return dut_.wb_reserve_index;
}

// Producer ports transfer once per cycle; the circuit checks ownership and rejects duplicate results.
// 每个生产端口每拍传输一次；电路检查归属并拒绝重复结果。
template<class C, uint32_t S> bool RtlWriteback<C,S>::complete(uint32_t entry, uint32_t pipe, const RegisterVector& values) {
    if (entry >= 32 || pipe >= 7) throw std::out_of_range("writeback_completion");
    if (sealed_ || dut_.wb_reserve_valid) throw std::runtime_error("writeback_completion_phase");
    const auto bit = 1u << pipe;
    if (dut_.wb_complete_valid & bit) return false;
    if (pipe == 0 && dut_.integer_output_valid) return false;
    if (pipe == 1 && dut_.floating_output_valid) return false;
    const auto shift = pipe*5;
    dut_.wb_complete_entries = (dut_.wb_complete_entries & ~(uint64_t{31} << shift)) | (uint64_t{entry} << shift);
    for (uint32_t lane = 0; lane < 32; ++lane) dut_.wb_complete_data[pipe*32+lane] = values[lane];
    dut_.wb_complete_valid |= bit; dut_.eval();
    if (dut_.wb_complete_error & bit) {
        dut_.wb_complete_valid &= ~bit; dut_.eval(); throw std::runtime_error("writeback_completion_rejected");
    }
    return true;
}

template<class C, uint32_t S> bool RtlWriteback<C,S>::selected(uint32_t entry) const {
    if (entry >= 32) throw std::out_of_range("writeback_entry");
    return sealed_ && ((dut_.wb_retire_ready & ~dut_.wb_acknowledge) & (1u << entry));
}

// Sample the actual retirement bus; its acknowledged data also flows directly into the RF circuit.
// 采样实际退休总线；确认后的同一份数据也直接流入寄存器堆电路。
template<class C, uint32_t S> RegisterVector RtlWriteback<C,S>::take(uint32_t entry) {
    if (!selected(entry) || dut_.wb_reserve_valid || retirement_closed_) throw std::runtime_error("writeback_not_selected");
    if ((dut_.wb_retire_valid & 16) && ((dut_.wb_retire_entries >> 20) & 31u) == entry && dut_.predicate_write_valid)
        throw std::runtime_error("predicate_write_port_busy");
    RegisterVector result{};
    for (uint32_t port = 0; port < 5; ++port)
        if ((dut_.wb_retire_valid & (1u << port)) && ((dut_.wb_retire_entries >> (port*5)) & 31u) == entry)
            for (uint32_t lane = 0; lane < 32; ++lane) result[lane] = dut_.wb_retire_data[port*32+lane];
    dut_.wb_acknowledge |= 1u << entry; dut_.eval();
    if (dut_.wb_acknowledge_error) throw std::runtime_error("writeback_acknowledge_rejected");
    return result;
}

template<class C, uint32_t S> void RtlWriteback<C,S>::sample() {
    ++stats_.clocks; stats_.active_clocks += bool(dut_.wb_clock_enable);
    stats_.reserved += bool(dut_.wb_reserve_valid && dut_.wb_reserve_ready);
    stats_.completed += std::popcount(uint32_t{dut_.wb_complete_accept});
    stats_.retired += std::popcount(uint32_t{dut_.wb_acknowledge & dut_.wb_retire_ready});
}

template<class C, uint32_t S> void RtlWriteback<C,S>::clear() {
    if (dut_.wb_reserve_valid || dut_.wb_complete_valid || dut_.wb_acknowledge) {
        dut_.wb_reserve_valid = 0; dut_.wb_complete_valid = 0; dut_.wb_acknowledge = 0; dut_.eval();
    }
    sealed_ = false; retirement_closed_ = false;
}
}

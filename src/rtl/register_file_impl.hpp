#pragma once
#include "simt/register_file.hpp"
#include <bit>
#include <stdexcept>

namespace simt::hw::detail {
template<class Circuit, uint32_t Slots> class RtlRegisterFile : public RegisterFile {
public:
    explicit RtlRegisterFile(uint32_t banks) {
        if (banks != 1 && banks != 2 && banks != 4) throw std::runtime_error("register_rtl_invalid_port_groups");
        dut_.port_groups = banks;
        dut_.clk = 0; dut_.reset = 1; dut_.read_valid = 0; dut_.write_valid = 0; dut_.clear_slots = 0;
        edge(); dut_.reset = 0; dut_.eval();
    }
    ~RtlRegisterFile() override { dut_.final(); }
    void clear(uint32_t slot) override;
    std::optional<RegisterVector> read(uint32_t slot, uint32_t reg) override;
    bool writable(uint32_t reg) override;
    void write(uint32_t slot, uint32_t reg, uint32_t mask, const RegisterVector& values) override;
    void tick() override;
    uint32_t port_groups() const override { return dut_.port_groups; }
    uint32_t slots() const override { return Slots; }
    const RegisterFileStats& stats() const override { return stats_; }
protected:
    Circuit dut_;
    RegisterFileStats stats_;
    static constexpr uint32_t row_bits = std::countr_zero(Slots)+4;
    bool collection_active() const {
        if constexpr (requires { dut_.collect_enable; }) return dut_.collect_enable;
        return false;
    }
    uint32_t read_events() const {
        if constexpr (requires { dut_.read_events; }) return dut_.read_events;
        return dut_.read_valid;
    }
    bool storage_clock_enabled() const {
        if constexpr (requires { dut_.register_clock_enable; }) return dut_.register_clock_enable;
        return dut_.clock_enable;
    }
    void edge() { dut_.clk = 1; dut_.eval(); dut_.clk = 0; dut_.eval(); }
    void validate(uint32_t slot, uint32_t reg) const;
    void query(uint32_t reg) { dut_.query_register = reg; dut_.eval(); }
};

template<class C, uint32_t S> void RtlRegisterFile<C,S>::validate(uint32_t slot, uint32_t reg) const {
    if (slot >= S || reg >= 64) throw std::out_of_range("register_file_address");
}

// Clear validity on physical slot reuse; data storage remains untouched and cannot leak through reads.
// 物理槽复用时清除有效位；数据存储保持不动，但旧值不能通过读取泄漏。
template<class C, uint32_t S> void RtlRegisterFile<C,S>::clear(uint32_t slot) {
    validate(slot, 0);
    if (dut_.read_valid || dut_.write_valid || collection_active()) throw std::runtime_error("register_clear_after_port_use");
    dut_.clear_slots |= uint32_t{1} << slot; dut_.eval();
}

// Accepted reads consume a real vector port and copy only that port's circuit-produced data.
// 已接受读取占用实际向量端口，只复制该端口由电路产生的数据。
template<class C, uint32_t S> std::optional<RegisterVector> RtlRegisterFile<C,S>::read(uint32_t slot, uint32_t reg) {
    validate(slot, reg);
    if (reg == 63) return RegisterVector{};
    query(reg);
    if (!dut_.read_available) return {};
    const auto bank = reg%4, shift = bank*row_bits;
    const auto mask = (uint32_t{1} << row_bits)-1;
    dut_.read_rows = (dut_.read_rows & ~(uint64_t{mask} << shift)) | (uint64_t{slot*16+reg/4} << shift);
    dut_.read_valid |= 1u << bank; dut_.eval();
    RegisterVector result;
    for (uint32_t lane = 0; lane < 32; ++lane) result[lane] = dut_.read_data[bank*32+lane];
    return result;
}

template<class C, uint32_t S> bool RtlRegisterFile<C,S>::writable(uint32_t reg) {
    validate(0, reg); query(reg); return dut_.write_available;
}

// Pending write pins provide same-cycle bypass; only the next edge changes the data array.
// 待写引脚提供同拍旁路，只有下一时钟边沿才改变数据阵列。
template<class C, uint32_t S> void RtlRegisterFile<C,S>::write(uint32_t slot, uint32_t reg, uint32_t mask,
                                                                         const RegisterVector& values) {
    validate(slot, reg);
    if (reg == 63 || !mask) return;
    if (dut_.read_valid || collection_active()) throw std::runtime_error("register_write_after_operand_collection");
    if (!writable(reg)) throw std::runtime_error("register_file_write_port_busy");
    const auto bank = reg%4, shift = bank*row_bits, row_mask = (uint32_t{1} << row_bits)-1;
    dut_.write_rows = (dut_.write_rows & ~(uint64_t{row_mask} << shift)) | (uint64_t{slot*16+reg/4} << shift);
    dut_.write_masks[bank] = mask;
    for (uint32_t lane = 0; lane < 32; ++lane) dut_.write_data[bank*32+lane] = values[lane];
    dut_.write_valid |= 1u << bank; dut_.eval();
}

// Count physical port events before clearing their pins; observation queries never advance time.
// 清除引脚之前统计物理端口事件；观测查询不推进时间。
template<class C, uint32_t S> void RtlRegisterFile<C,S>::tick() {
    ++stats_.clocks;
    stats_.reads += std::popcount(read_events());
    stats_.writes += std::popcount(uint32_t{dut_.write_valid});
    stats_.cleared_slots += std::popcount(uint32_t{dut_.clear_slots});
    if (storage_clock_enabled()) ++stats_.active_clocks;
    if (dut_.clock_enable) edge();
    if (dut_.read_valid || dut_.write_valid || dut_.clear_slots) {
        dut_.read_valid = 0; dut_.write_valid = 0; dut_.clear_slots = 0; dut_.eval();
    }
}
}

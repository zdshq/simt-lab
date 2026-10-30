#include "simt/operand_collector.hpp"
#include "Vsimt_operands4.h"
#include "Vsimt_operands8.h"
#include "Vsimt_operands16.h"
#include "Vsimt_operands32.h"
#include "register_file_impl.hpp"
#include "writeback_impl.hpp"
#include "integer_impl.hpp"
#include "floating_impl.hpp"
#include "collective_impl.hpp"

namespace simt::hw {
namespace {
template<class Circuit, uint32_t Slots>
class RtlOperandUnit final : public detail::RtlRegisterFile<Circuit, Slots>, public OperandCollector {
    using Base = detail::RtlRegisterFile<Circuit, Slots>;
    using Base::dut_;
public:
    RtlOperandUnit(uint32_t banks, uint32_t collectors) : Base(banks), writeback_(dut_), integer_(dut_), floating_(dut_), collective_(dut_) {
        if (!collectors || collectors > 32) throw std::runtime_error("invalid_collector_limit");
        dut_.entry_limit = collectors; dut_.collect_enable = 0;
        dut_.allocate_valid = 0; dut_.release_entries = 0;
        dut_.dispatch_enable = 0; dut_.dispatch_accept = 0; dut_.dispatch_eligible = 0; dut_.eval();
    }
    OperandCollector* collector() override { return this; }
    Writeback* writeback() override { return &writeback_; }
    IntegerExecution* integer() override { return &integer_; }
    FloatingExecution* floating() override { return &floating_; }
    CollectiveExecution* collective() override { return &collective_; }
    bool available() override { return !dut_.allocate_valid && dut_.allocate_ready; }
    uint32_t allocate(uint32_t warp, std::span<const RegisterUse> sources, uint32_t pipe) override;
    std::vector<CollectorGrant> collect() override;
    bool ready(uint32_t entry) override;
    void dispatch(uint32_t eligible) override;
    bool selected(uint32_t entry) const override { return selected_port(entry).has_value(); }
    std::vector<CollectedOperand> take(uint32_t entry) override;
    void tick() override;
    uint32_t occupancy() const override { return dut_.occupancy; }
    uint32_t limit() const override { return dut_.entry_limit; }
    const CollectorStats& collector_stats() const override { return stats_; }
    void write(uint32_t slot, uint32_t reg, uint32_t mask, const RegisterVector& values) override {
        if ((writeback_.occupancy() || dut_.wb_reserve_valid) && reg != 63 && mask)
            throw std::runtime_error("writeback_owns_register_write_ports");
        if (serviced_ && reg != 63 && mask) throw std::runtime_error("register_write_after_operand_collection");
        Base::write(slot, reg, mask, values);
    }
    void clear(uint32_t slot) override {
        if (serviced_) throw std::runtime_error("register_clear_after_operand_collection");
        Base::clear(slot);
    }
    std::optional<RegisterVector> read(uint32_t, uint32_t) override {
        throw std::runtime_error("collector_owns_register_read_ports");
    }
private:
    CollectorStats stats_;
    detail::RtlWriteback<Circuit, Slots> writeback_;
    detail::RtlInteger<Circuit> integer_;
    detail::RtlFloating<Circuit> floating_;
    detail::RtlCollective<Circuit, Slots> collective_;
    bool serviced_ = false, dispatched_ = false;
    std::optional<uint32_t> selected_port(uint32_t entry) const;
    uint32_t pending_count() const;
};

// A refused allocation is withdrawn without changing any existing collector or issuing a clock edge.
// 被拒绝的分配撤销引脚，不改变已有收集器，也不产生时钟边沿。
template<class C, uint32_t S> uint32_t RtlOperandUnit<C,S>::allocate(uint32_t warp, std::span<const RegisterUse> sources, uint32_t pipe) {
    if (warp >= S || sources.size() > 3 || pipe >= 7) throw std::out_of_range("collector_descriptor");
    if (!available()) throw std::runtime_error("collector_allocation_capacity");
    dut_.allocate_pipe = pipe; dut_.allocate_warp = warp; dut_.allocate_count = sources.size(); dut_.allocate_registers = 0;
    for (uint32_t source = 0; source < sources.size(); ++source) {
        if (sources[source].reg >= 64) throw std::out_of_range("collector_register");
        dut_.allocate_registers |= sources[source].reg << (source*6);
        dut_.allocate_masks[source] = sources[source].mask;
    }
    dut_.allocate_valid = 1; dut_.eval();
    if (!dut_.allocate_ready) {
        dut_.allocate_valid = 0; dut_.eval(); throw std::runtime_error("invalid_collector_descriptor");
    }
    return dut_.allocate_index;
}

template<class C, uint32_t S> uint32_t RtlOperandUnit<C,S>::pending_count() const {
    uint32_t count = 0;
    for (uint32_t word = 0; word < 3; ++word) count += std::popcount(uint32_t{dut_.waiting_sources[word]});
    return count;
}

// Only circuit grants release scoreboard readers; the data itself stays on the internal RTL wires.
// 只有电路授权才释放 scoreboard 读者；数据本身始终留在 RTL 内部连线上。
template<class C, uint32_t S> std::vector<CollectorGrant> RtlOperandUnit<C,S>::collect() {
    if (serviced_) throw std::runtime_error("collector_already_serviced");
    if (dut_.allocate_valid) throw std::runtime_error("collector_collection_after_allocation");
    writeback_.close_retirement();
    serviced_ = true;
    // Circuit pending bits prove that an empty request set needs no arbitration or data capture.
    // 电路待处理位证明空请求集合不需要仲裁或捕获数据。
    if (!pending_count()) return {};
    dut_.collect_enable = 1; dut_.eval();
    std::vector<CollectorGrant> grants;
    for (uint32_t bank = 0; bank < 4; ++bank)
        if (dut_.read_events & (1u << bank))
            grants.push_back({uint32_t(dut_.grant_entries >> (bank*5)) & 31u,
                              uint32_t(dut_.grant_sources >> (bank*2)) & 3u,
                              uint32_t(dut_.grant_registers >> (bank*6)) & 63u, dut_.grant_masks[bank]});
    stats_.bank_stalls += pending_count()-grants.size();
    return grants;
}

template<class C, uint32_t S> bool RtlOperandUnit<C,S>::ready(uint32_t entry) {
    if (entry >= 32) throw std::out_of_range("collector_entry");
    return (dut_.ready_entries & ~(dut_.release_entries | dut_.dispatch_releases)) & (1u << entry);
}

// Eligibility reports external resources; the circuit alone picks the oldest request on each port.
// 准入位反映外部资源，只有电路负责选择每个端口的最老请求。
template<class C, uint32_t S> void RtlOperandUnit<C,S>::dispatch(uint32_t eligible) {
    if (!serviced_ || dispatched_ || dut_.allocate_valid) throw std::runtime_error("collector_dispatch_phase");
    dispatched_ = true;
    if (!(dut_.ready_entries & eligible)) return;
    dut_.dispatch_eligible = eligible; dut_.dispatch_enable = 1; dut_.eval();
}

template<class C, uint32_t S> std::optional<uint32_t> RtlOperandUnit<C,S>::selected_port(uint32_t entry) const {
    if (entry >= 32) throw std::out_of_range("collector_entry");
    for (uint32_t port = 0; port < 7; ++port)
        if ((dut_.dispatch_valid & ~dut_.dispatch_accept_events) & (1u << port))
            if (((dut_.dispatch_entries >> (port*5)) & 31u) == entry) return port;
    return {};
}

// Sample the chosen physical execution bus, then acknowledge that port exactly once this cycle.
// 采样所选物理执行总线，然后在本拍仅确认该端口一次。
template<class C, uint32_t S> std::vector<CollectedOperand> RtlOperandUnit<C,S>::take(uint32_t entry) {
    const auto port = selected_port(entry);
    if (!serviced_ || !dispatched_ || dut_.allocate_valid || !port) throw std::runtime_error("collector_not_selected");
    std::vector<CollectedOperand> result;
    const auto count = (dut_.dispatch_counts >> (*port*2)) & 3u;
    for (uint32_t source = 0; source < count; ++source) {
        const auto shift = *port*18+source*6;
        uint64_t packed = dut_.dispatch_registers[shift/32];
        if (shift%32 > 26) packed |= uint64_t{dut_.dispatch_registers[shift/32+1]} << 32;
        CollectedOperand operand; operand.reg = (packed >> (shift%32)) & 63u;
        for (uint32_t lane = 0; lane < 32; ++lane)
            operand.values[lane] = dut_.dispatch_data[*port*96+source*32+lane];
        result.push_back(operand);
    }
    dut_.dispatch_accept |= 1u << *port; dut_.eval();
    if (dut_.dispatch_error) throw std::runtime_error("collector_dispatch_rejected");
    return result;
}

template<class C, uint32_t S> void RtlOperandUnit<C,S>::tick() {
    ++stats_.clocks;
    stats_.active_clocks += bool(dut_.collector_clock_enable);
    stats_.allocated += bool(dut_.allocate_valid && dut_.allocate_ready);
    stats_.released += std::popcount(uint32_t{dut_.release_entries | dut_.dispatch_releases});
    stats_.dispatches += std::popcount(static_cast<uint32_t>(dut_.dispatch_accept_events & dut_.dispatch_valid));
    stats_.dispatch_waits += std::popcount(uint32_t{dut_.ready_entries & ~dut_.dispatch_releases});
    stats_.reads += std::popcount(uint32_t{dut_.read_events});
    writeback_.sample();
    integer_.sample();
    floating_.sample();
    collective_.sample();
    Base::tick();
    collective_.clear();
    floating_.clear();
    integer_.clear();
    writeback_.clear();
    if (dut_.allocate_valid || dut_.release_entries || dut_.collect_enable || dut_.dispatch_enable || dut_.dispatch_accept) {
        dut_.allocate_valid = 0; dut_.release_entries = 0; dut_.collect_enable = 0;
        dut_.dispatch_enable = 0; dut_.dispatch_accept = 0; dut_.eval();
    }
    serviced_ = false; dispatched_ = false;
}
}

std::unique_ptr<RegisterFile> make_rtl_operand_unit(uint32_t partitions, uint32_t banks, uint32_t collectors) {
    switch (partitions) {
    case 1: return std::make_unique<RtlOperandUnit<Vsimt_operands32,32>>(banks, collectors);
    case 2: return std::make_unique<RtlOperandUnit<Vsimt_operands16,16>>(banks, collectors);
    case 4: return std::make_unique<RtlOperandUnit<Vsimt_operands8,8>>(banks, collectors);
    case 8: return std::make_unique<RtlOperandUnit<Vsimt_operands4,4>>(banks, collectors);
    default: throw std::runtime_error("register_rtl_requires_power_of_two_partitions");
    }
}
}

#include "simt/hardware_memory.hpp"
#include "Vsimt_shared_memory.h"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
struct Region { uint32_t base, bytes; };
struct Transfer { Ticket ticket; Memory* observation; };
class RtlShared final : public SharedPort {
public:
    explicit RtlShared(SharedConfig config);
    ~RtlShared() override { dut_.final(); }
    std::optional<Ticket> submit(const WarpAccess& access,Memory* backing) override;
    void tick() override;
    std::optional<std::array<uint32_t,32>> take(Ticket ticket) override;
    uint64_t cycle() const override { return stats_.clocks; }
    const SharedStats& stats() const override { return stats_; }
    std::string_view name() const override { return "rtl"; }
    size_t outstanding() const override;
    void attach(Memory& memory,uint32_t base,uint32_t bytes) override;
    void detach(Memory& memory) override;
private:
    Vsimt_shared_memory dut_;
    SharedStats stats_;
    std::map<Memory*,Region> regions_;
    std::array<std::optional<Transfer>,8> transfers_;
    Ticket next_=1;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void clear();
    void drive(const WarpAccess& access,uint32_t base);
    void observe_writes();
    void sample_events();
    void observe_word(uint32_t address,uint32_t mask,uint32_t value);
};
void RtlShared::clear() {
    dut_.request_valid=0; dut_.acknowledge=0;
    for (auto& word : dut_.clear_rows) word=0;
}
RtlShared::RtlShared(SharedConfig config) {
    if (config.bytes!=65536 || config.banks!=32 || !config.capacity || config.capacity>8 || !config.latency || !config.atomic_latency)
        throw std::runtime_error("invalid_shared_rtl_configuration");
    clear(); dut_.capacity=config.capacity; dut_.response_latency=config.latency; dut_.atomic_latency=config.atomic_latency;
    dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}
size_t RtlShared::outstanding() const {
    size_t count=0; for (const auto& transfer : transfers_) count+=bool(transfer); return count;
}

// Region allocation only clears hardware validity; it never imports bytes from the observation object.
// 区域分配仅清除硬件有效位，绝不从观测对象导入字节。
void RtlShared::attach(Memory& memory,uint32_t base,uint32_t bytes) {
    const uint64_t reserved=(uint64_t{bytes}+127)/128*128;
    memory.check(0,bytes);
    if (base%128 || base>65536 || reserved>65536-base || regions_.contains(&memory))
        throw std::runtime_error("invalid_shared_rtl_region");
    for (const auto& [storage,region] : regions_) {
        static_cast<void>(storage); const auto end=region.base+(region.bytes+127)/128*128;
        if (reserved && region.bytes && base<end && region.base<base+reserved)
            throw std::runtime_error("overlapping_shared_rtl_region");
    }
    memory.clear(0,bytes); regions_.emplace(&memory,Region{base,bytes});
    for (uint32_t row=base/128; row<(base+reserved)/128; ++row) dut_.clear_rows[row/32]|=uint32_t{1}<<(row%32);
    dut_.eval();
    if (dut_.error) throw std::runtime_error("shared_rtl_region_clear_error");
}
void RtlShared::detach(Memory& memory) {
    for (const auto& transfer : transfers_)
        if (transfer && transfer->observation==&memory) throw std::runtime_error("shared_rtl_region_busy");
    if (!regions_.erase(&memory)) throw std::runtime_error("unknown_shared_rtl_region");
}
void RtlShared::drive(const WarpAccess& access,uint32_t base) {
    dut_.request_write=access.write; dut_.request_atomic=bool(access.atomic); dut_.request_mode=access.atomic.value_or(0);
    dut_.request_width=access.width==1 ? 0 : access.width==2 ? 1 : 2; dut_.request_mask=access.mask;
    for (uint32_t lane=0; lane<32; ++lane) {
        const auto address=(access.mask & (uint32_t{1}<<lane)) ? base+access.address[lane] : 0;
        if (lane%2==0) dut_.request_addresses[lane/2]=0;
        dut_.request_addresses[lane/2]|=static_cast<uint32_t>(address)<<(lane%2*16);
        dut_.request_values[lane]=access.value[lane]; dut_.request_replacements[lane]=access.replacement[lane];
    }
}
std::optional<Ticket> RtlShared::submit(const WarpAccess& access,Memory* backing) {
    const auto region=regions_.find(backing);
    if (region==regions_.end()) throw std::runtime_error("unknown_shared_rtl_region");
    validate_access(access,*backing);
    for (uint32_t lane=0; lane<32; ++lane)
        if ((access.mask & (uint32_t{1}<<lane)) && access.address[lane]+access.width>region->second.bytes)
            throw std::runtime_error("memory_out_of_bounds");
    if (dut_.request_valid || !dut_.request_ready) { ++stats_.queue_stalls; return {}; }
    drive(access,region->second.base); dut_.request_index=dut_.request_slot; dut_.request_valid=1; dut_.eval();
    if (dut_.error) { dut_.request_valid=0; dut_.eval(); throw std::runtime_error("shared_rtl_request_error"); }
    const Ticket ticket=next_++; transfers_[dut_.request_slot]=Transfer{ticket,backing}; return ticket;
}
std::optional<std::array<uint32_t,32>> RtlShared::take(Ticket ticket) {
    for (uint32_t slot=0; slot<8; ++slot) {
        if (!transfers_[slot] || transfers_[slot]->ticket!=ticket) continue;
        if (!(dut_.completed & (1u<<slot)) || (dut_.request_valid && dut_.request_slot==slot)) return {};
        dut_.query_slot=slot; dut_.eval(); std::array<uint32_t,32> result{};
        for (uint32_t lane=0; lane<32; ++lane) result[lane]=dut_.query_data[lane];
        dut_.acknowledge|=1u<<slot; dut_.eval(); transfers_[slot].reset(); return result;
    }
    throw std::runtime_error("invalid_shared_ticket");
}

// Snapshots follow actual bank write ports; future reads never use these copied bytes.
// 快照跟随实际 bank 写端口更新，后续读取绝不使用这些复制字节。
void RtlShared::observe_word(uint32_t address,uint32_t mask,uint32_t value) {
    for (const auto& [memory,region] : regions_) {
        if (address<region.base || address>=region.base+region.bytes) continue;
        for (uint32_t byte=0; byte<4; ++byte)
            if (mask & (1u<<byte)) memory->write(address-region.base+byte,1,(value>>(byte*8))&255);
        return;
    }
    throw std::runtime_error("shared_rtl_write_without_owner");
}
void RtlShared::observe_writes() {
    for (uint32_t bank=0; bank<32; ++bank) if (dut_.bank_writes & (1u<<bank)) {
        const auto bit=bank*9, word=bit/32, shift=bit%32;
        uint64_t row=dut_.write_rows[word];
        if (shift>23) row|=uint64_t{dut_.write_rows[word+1]}<<32;
        const auto address=static_cast<uint32_t>((row>>shift)&511)*128+bank*4;
        observe_word(address,(dut_.write_masks[bank/8]>>(bank%8*4))&15,dut_.write_values[bank]);
    }
}
void RtlShared::sample_events() {
    stats_.accepted+=bool(dut_.request_valid && dut_.request_ready);
    stats_.consumed+=std::popcount(uint32_t{dut_.acknowledge});
    stats_.bank_services+=std::popcount(uint32_t{dut_.bank_accepted})+std::popcount(uint32_t{dut_.atomic_completed});
    stats_.atomic_started+=std::popcount(uint32_t{dut_.atomic_started});
    stats_.atomic_completed+=std::popcount(uint32_t{dut_.atomic_completed});
    for (auto mask : dut_.blocked_lanes) stats_.bank_stalls+=std::popcount(uint32_t{mask});
    for (uint32_t bank=0; bank<32; ++bank)
        if (dut_.bank_accepted & (1u<<bank)) stats_.broadcasts+=std::popcount(uint32_t{dut_.served_lanes[bank]})-1;
}
void RtlShared::tick() {
    if (dut_.error) throw std::runtime_error("shared_rtl_protocol_error");
    ++stats_.clocks; sample_events(); observe_writes();
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear(); dut_.eval();
}
}
std::unique_ptr<SharedPort> make_rtl_shared_engine(SharedConfig config) { return std::make_unique<RtlShared>(config); }
}

#include "simt/dram_engine.hpp"
#include "Vdram_controller.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>

namespace simt::hw {
namespace {
void write_words(WData* words,const Sector& data) {
    for (uint32_t word=0; word<8; ++word) {
        uint32_t value=0;
        for (uint32_t byte=0; byte<4; ++byte) value|=uint32_t{data[word*4+byte]}<<(byte*8);
        words[word]=value;
    }
}
Sector read_words(const WData* words) {
    Sector data{};
    for (uint32_t word=0; word<8; ++word)
        for (uint32_t byte=0; byte<4; ++byte) data[word*4+byte]=static_cast<uint8_t>(words[word]>>(byte*8));
    return data;
}

// Data cells remain in external memory; the circuit decides when each sector transfer happens.
// 数据单元保留在外部存储中；电路决定每个 sector 何时传输。
class RtlDram final : public DramPort {
public:
    RtlDram(MemoryBacking& memory,const DramConfig& config);
    ~RtlDram() override { dut_.final(); }
    std::optional<Ticket> submit(const Packet& packet) override;
    std::optional<Sector> take(Ticket ticket) override;
    void tick() override;
    const DramStats& stats() const override { return stats_; }
    std::string_view name() const override { return "rtl"; }
private:
    MemoryBacking& memory_;
    Vdram_controller dut_;
    DramStats stats_;
    std::map<Ticket,uint32_t> slots_;
    void edge();
    void settle();
    void transfer();
};

RtlDram::RtlDram(MemoryBacking& memory,const DramConfig& config) : memory_(memory) {
    if (config.banks!=8 || config.row_bytes!=2048 || config.capacity>64 || !config.capacity)
        throw std::runtime_error("dram_rtl_requires_fixed_geometry");
    dut_.rcd=config.rcd; dut_.cas=config.cas; dut_.rp=config.rp; dut_.burst=config.burst;
    dut_.reset=1; edge(); edge(); dut_.reset=0; settle();
}

void RtlDram::edge() {
    dut_.clk=0; dut_.eval();
    dut_.clk=1; dut_.eval();
    dut_.clk=0; dut_.eval();
}

void RtlDram::settle() {
    dut_.eval();
    if (dut_.error) throw std::runtime_error("dram_protocol_error "+std::to_string(dut_.error));
}

// The single request pin holds one packet per clock; a second packet waits for the next clock.
// 单个请求引脚每拍保持一个包；第二个包等待下一拍。
std::optional<Ticket> RtlDram::submit(const Packet& packet) {
    check_packet(packet);
    if (packet.write) memory_.check_write(packet.address,sector_bytes);
    else memory_.check(packet.address,sector_bytes);
    dut_.eval();
    if (dut_.request_valid || !dut_.request_ready) { ++stats_.queue_stalls; return {}; }
    const auto ticket=reserve();
    dut_.request_valid=1; dut_.request_write=packet.write; dut_.request_address=packet.address;
    dut_.request_mask=packet.byte_mask; write_words(dut_.request_data,packet.data); settle();
    slots_[ticket]=dut_.request_slot;
    if (packet.write) ++stats_.writes; else ++stats_.reads;
    if (!packet.write && memory_.instruction_address(packet.address)) ++stats_.instruction_reads;
    return ticket;
}

std::optional<Sector> RtlDram::take(Ticket ticket) {
    const auto found=slots_.find(ticket);
    if (found==slots_.end()) throw std::runtime_error("invalid_memory_ticket");
    const uint64_t bit=uint64_t{1}<<found->second;
    if (!(dut_.completed & bit) || (dut_.acknowledge & bit)) return {};
    dut_.query_slot=found->second; dut_.eval();
    const auto data=read_words(dut_.query_data);
    dut_.acknowledge|=bit; settle();
    slots_.erase(found); responses_.erase(ticket);
    return data;
}

// Writes reach the external cells before the returned sector is sampled, matching a write-then-read burst.
// 写入先到达外部单元，再采样返回 sector，等同一次先写后读的突发。
void RtlDram::transfer() {
    dut_.eval();
    if (!dut_.transfer_valid) return;
    const auto data=read_words(dut_.transfer_data);
    Sector result{};
    for (uint32_t byte=0; byte<sector_bytes; ++byte) {
        const auto address=dut_.transfer_address+byte;
        if (dut_.transfer_write && (dut_.transfer_mask & (uint32_t{1}<<byte))) memory_.write(address,1,data[byte]);
        result[byte]=static_cast<uint8_t>(memory_.read(address,1));
    }
    write_words(dut_.transfer_result,result); dut_.eval();
}

void RtlDram::tick() {
    ++cycle_; settle(); transfer();
    if (dut_.started) { if (dut_.started_hit) ++stats_.row_hits; else ++stats_.row_misses; }
    edge();
    dut_.request_valid=0; dut_.acknowledge=0; settle();
}
}

std::unique_ptr<DramPort> make_rtl_dram(MemoryBacking& memory,const DramConfig& config) {
    return std::make_unique<RtlDram>(memory,config);
}
}

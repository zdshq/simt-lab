#include "simt/dma_engine.hpp"
#include "Vsimt_dma_engine.h"
#include <algorithm>
#include <stdexcept>

namespace simt::hw {
namespace {
// Each packed address is 33 bits, preserving carries beyond a 32-bit command base.
// 每个打包地址为 33 位，保留超过 32 位命令基址的进位。
template<class Words> uint64_t address(const Words& words,uint32_t lane) {
    const auto shift=lane*33; const uint64_t bits=uint64_t{words[shift/32]}|(uint64_t{words[shift/32+1]}<<32);
    return (bits>>(shift%32))&0x1ffffffffull;
}
class RtlDmaEngine final : public DmaEngine {
public:
    RtlDmaEngine(uint32_t commands,uint32_t fragments,uint32_t chunk);
    ~RtlDmaEngine() override { dut_.final(); }
    bool available() const override { return dut_.command_ready && !dut_.command_valid; }
    void submit(const DmaDescriptor& command) override;
    std::optional<DmaAccess> request(bool write) const override;
    void accept(bool write) override;
    DmaReturn returned_read(uint32_t slot,const std::array<uint32_t,32>& data) override;
    DmaReturn returned_write(uint32_t slot) override;
    std::optional<DmaCompletion> completion() const override;
    void claim() override;
    void release(const DmaCompletion& completion) override;
    uint32_t commands() const override { return dut_.command_occupancy; }
    uint32_t fragments() const override { return dut_.fragment_occupancy; }
    void tick() override;
    const DmaEngineStats& stats() const override { return stats_; }
private:
    Vsimt_dma_engine dut_;
    DmaEngineStats stats_;
    void clear();
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
};
void RtlDmaEngine::clear() {
    dut_.command_valid=0; dut_.read_ready=0; dut_.write_ready=0; dut_.read_response_valid=0;
    dut_.write_response_valid=0; dut_.completion_ready=0; dut_.release_valid=0;
}
RtlDmaEngine::RtlDmaEngine(uint32_t commands,uint32_t fragments,uint32_t chunk) {
    if (!commands || commands>64 || !fragments || fragments>64 || !chunk || chunk>128)
        throw std::out_of_range("invalid_dma_configuration");
    clear(); dut_.clk=0; dut_.command_limit=commands; dut_.fragment_limit=fragments; dut_.chunk_bytes=chunk;
    dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}
void RtlDmaEngine::submit(const DmaDescriptor& c) {
    if (!available()) throw std::runtime_error("dma_command_phase");
    dut_.command_id=c.id; dut_.command_cta=c.cta; dut_.command_source=c.source; dut_.command_destination=c.destination;
    dut_.command_barrier=c.barrier; dut_.command_phase=c.phase; dut_.command_bytes=c.bytes;
    dut_.command_valid=1; dut_.eval();
    if (dut_.command_accept) return;
    const auto error=dut_.error; dut_.command_valid=0; dut_.eval();
    throw std::runtime_error(error==2 ? "invalid_async_copy_size" : error==3 ? "duplicate_dma_id" : "dma_command_rejected");
}

std::optional<DmaAccess> RtlDmaEngine::request(bool write) const {
    if (write ? !dut_.write_valid || dut_.write_ready : !dut_.read_valid || dut_.read_ready) return {};
    DmaAccess r; r.slot=write ? dut_.write_slot : dut_.read_slot; r.id=write ? dut_.write_copy : dut_.read_copy;
    r.cta=write ? dut_.write_cta : dut_.read_cta; r.access.write=write;
    r.access.mask=write ? dut_.write_mask : dut_.read_mask; r.access.width=(write ? dut_.write_word : dut_.read_word) ? 4 : 1;
    for (uint32_t lane=0; lane<32; ++lane) {
        r.access.address[lane]=write ? address(dut_.write_addresses,lane) : address(dut_.read_addresses,lane);
        if (write) r.access.value[lane]=dut_.write_data[lane];
    }
    return r;
}
void RtlDmaEngine::accept(bool write) {
    if (!request(write)) throw std::runtime_error("dma_request_phase");
    if (write) dut_.write_ready=1; else dut_.read_ready=1;
    dut_.eval();
}
DmaReturn RtlDmaEngine::returned_read(uint32_t slot,const std::array<uint32_t,32>& data) {
    if (slot>=64 || dut_.read_response_valid) throw std::runtime_error("dma_read_return_phase");
    dut_.read_response_slot=slot; dut_.read_response_valid=1;
    for (uint32_t lane=0; lane<32; ++lane) dut_.read_response_data[lane]=data[lane];
    dut_.eval();
    if (!dut_.read_response_ready) { dut_.read_response_valid=0; dut_.eval(); throw std::runtime_error("dma_invalid_read_return"); }
    return {dut_.read_response_copy,dut_.read_response_bytes};
}
DmaReturn RtlDmaEngine::returned_write(uint32_t slot) {
    if (slot>=64 || dut_.write_response_valid) throw std::runtime_error("dma_write_return_phase");
    dut_.write_response_slot=slot; dut_.write_response_valid=1; dut_.eval();
    if (!dut_.write_response_ready) { dut_.write_response_valid=0; dut_.eval(); throw std::runtime_error("dma_invalid_write_return"); }
    return {dut_.write_response_copy,dut_.write_response_bytes};
}
std::optional<DmaCompletion> RtlDmaEngine::completion() const {
    if (!dut_.completion_valid || dut_.completion_ready) return {};
    return DmaCompletion{dut_.completion_slot,dut_.completion_id,dut_.completion_cta,dut_.completion_barrier,dut_.completion_phase,dut_.completion_bytes};
}
void RtlDmaEngine::claim() {
    if (!completion()) throw std::runtime_error("dma_completion_phase");
    dut_.completion_ready=1; dut_.eval();
}
void RtlDmaEngine::release(const DmaCompletion& c) {
    if (c.slot>=64 || dut_.release_valid) throw std::runtime_error("dma_release_phase");
    dut_.release_slot=c.slot; dut_.release_id=c.id; dut_.release_valid=1; dut_.eval();
    if (!dut_.release_ready) { dut_.release_valid=0; dut_.eval(); throw std::runtime_error("dma_invalid_release"); }
}

// One model tick samples actual handshakes, advances one edge, then withdraws all input events.
// 一个模型 tick 采样实际握手，推进一个边沿，然后撤销全部输入事件。
void RtlDmaEngine::tick() {
    ++stats_.clocks;
    stats_.submitted+=bool(dut_.command_accept); stats_.released+=bool(dut_.release_valid && dut_.release_ready);
    stats_.fragments+=bool(dut_.fragment_allocated); stats_.fragment_stalls+=bool(dut_.fragment_stalled);
    stats_.reads+=bool(dut_.read_valid && dut_.read_ready); stats_.writes+=bool(dut_.write_valid && dut_.write_ready);
    if (dut_.read_response_valid && dut_.read_response_ready) { ++stats_.read_returns; stats_.read_bytes+=dut_.read_response_bytes; }
    if (dut_.write_response_valid && dut_.write_response_ready) { ++stats_.write_returns; stats_.write_bytes+=dut_.write_response_bytes; }
    stats_.completion_claims+=bool(dut_.completion_valid && dut_.completion_ready);
    stats_.read_stalls+=bool(dut_.read_valid && !dut_.read_ready); stats_.write_stalls+=bool(dut_.write_valid && !dut_.write_ready);
    stats_.completion_stalls+=bool(dut_.completion_valid && !dut_.completion_ready);
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    stats_.peak_commands=std::max(stats_.peak_commands,uint64_t{commands()});
    stats_.peak_fragments=std::max(stats_.peak_fragments,uint64_t{fragments()}); clear(); dut_.eval();
}
}
std::unique_ptr<DmaEngine> make_dma_engine(uint32_t commands,uint32_t fragments,uint32_t chunk) {
    return std::make_unique<RtlDmaEngine>(commands,fragments,chunk);
}
}

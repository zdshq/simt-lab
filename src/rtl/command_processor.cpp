#include "simt/command_processor.hpp"
#include "Vcommand_processor.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace simt::hw {
namespace {
constexpr uint32_t reg_put=0, reg_get=1, reg_notified=2;

// The adapter only drives pins and reads results; pointers and occupancy stay in the circuit.
// 适配器只驱动引脚并读取结果；指针和占用状态保存在电路中。
class RtlCommandProcessor final : public CommandProcessor {
public:
    explicit RtlCommandProcessor(uint32_t capacity);
    ~RtlCommandProcessor() override { dut_.final(); }
    void ring_doorbell(uint64_t put) override;
    uint64_t published() const override { return read(reg_put); }
    uint64_t fetched() const override { return read(reg_get); }
    uint64_t notified() const override { return read(reg_notified); }
    std::optional<uint32_t> fetch_slot() const override;
    uint32_t lookup(uint32_t handle) override;
    void accept_fetch(uint32_t handle,EngineKind kind,uint32_t wait) override;
    void offer_external(uint32_t pending,uint32_t failed) override;
    void offer_node(const std::optional<CpNode>& node) override;
    std::optional<CpDispatch> dispatch() const override;
    std::optional<uint32_t> select_graph(uint32_t pending) override;
    void complete(uint32_t slot,bool failed) override;
    void release(EngineKind kind) override;
    bool busy(EngineKind kind) const override;
    void tick() override;
    const CpStats& stats() const override { return stats_; }
private:
    mutable Vcommand_processor dut_;
    CpStats stats_;
    uint32_t read(uint32_t address) const;
    void settle();
    void edge();
    void clear_event_pins();
    void count_events();
};

RtlCommandProcessor::RtlCommandProcessor(uint32_t capacity) {
    dut_.capacity=capacity; dut_.reset=1; edge(); edge(); dut_.reset=0;
    clear_event_pins(); settle();
}

// A read drives only the address; held doorbell pins continue to select the write path.
// 读取只驱动地址；保持中的门铃引脚继续选择写路径。
uint32_t RtlCommandProcessor::read(uint32_t address) const {
    const auto saved=dut_.mmio_address;
    const bool held=dut_.mmio_valid && dut_.mmio_write;
    if (held && address==reg_put) return dut_.mmio_wdata;
    dut_.mmio_address=address; dut_.eval();
    const auto value=dut_.mmio_rdata;
    dut_.mmio_address=saved; dut_.eval();
    return value;
}

void RtlCommandProcessor::settle() {
    dut_.eval();
    if (dut_.error) throw std::runtime_error("command_processor_protocol_error "+std::to_string(dut_.error));
}

void RtlCommandProcessor::edge() {
    dut_.clk=0; dut_.eval();
    dut_.clk=1; dut_.eval();
    dut_.clk=0; dut_.eval();
}

void RtlCommandProcessor::clear_event_pins() {
    dut_.mmio_valid=0; dut_.mmio_write=0; dut_.mmio_address=0; dut_.mmio_wdata=0;
    dut_.fetch_accept=0; dut_.fetch_handle=0; dut_.fetch_kind=0; dut_.fetch_wait=0; dut_.lookup_handle=0;
    dut_.external_pending=0; dut_.external_failed=0; dut_.node_valid=0; dut_.node_cancel=0; dut_.node_kind=0;
    dut_.node_handle=0; dut_.graph_pending=0; dut_.complete_mask=0; dut_.failed_mask=0;
    dut_.compute_release=0; dut_.copy_release=0;
}

// The host write is held on the MMIO pins and commits on the next device clock edge.
// 主机写保持在 MMIO 引脚上，并在下一个设备时钟边沿提交。
void RtlCommandProcessor::ring_doorbell(uint64_t put) {
    dut_.mmio_valid=1; dut_.mmio_write=1; dut_.mmio_address=reg_put;
    dut_.mmio_wdata=static_cast<uint32_t>(put); settle();
}

std::optional<uint32_t> RtlCommandProcessor::fetch_slot() const {
    dut_.eval();
    if (!dut_.fetch_valid) return {};
    return dut_.fetch_slot;
}

uint32_t RtlCommandProcessor::lookup(uint32_t handle) {
    dut_.lookup_handle=handle; dut_.eval(); return dut_.lookup_mask;
}

void RtlCommandProcessor::accept_fetch(uint32_t handle,EngineKind kind,uint32_t wait) {
    dut_.fetch_accept=1; dut_.fetch_handle=handle; dut_.fetch_kind=static_cast<uint32_t>(kind);
    dut_.fetch_wait=wait; settle();
}

void RtlCommandProcessor::offer_external(uint32_t pending,uint32_t failed) {
    dut_.external_pending=pending; dut_.external_failed=failed; settle();
}

void RtlCommandProcessor::offer_node(const std::optional<CpNode>& node) {
    dut_.node_valid=node.has_value();
    dut_.node_handle=node ? node->handle : 0;
    dut_.node_kind=node ? static_cast<uint32_t>(node->kind) : 0;
    dut_.node_cancel=node && node->cancel;
    settle();
}

std::optional<CpDispatch> RtlCommandProcessor::dispatch() const {
    dut_.eval();
    if (!dut_.dispatch_valid) return {};
    return CpDispatch{bool(dut_.dispatch_node),bool(dut_.dispatch_cancel),dut_.dispatch_slot};
}

std::optional<uint32_t> RtlCommandProcessor::select_graph(uint32_t pending) {
    dut_.graph_pending=pending; settle();
    if (!dut_.graph_valid) return {};
    return dut_.graph_slot;
}

void RtlCommandProcessor::complete(uint32_t slot,bool failed) {
    dut_.complete_mask|=uint32_t{1}<<slot;
    if (failed) dut_.failed_mask|=uint32_t{1}<<slot;
    settle();
}

void RtlCommandProcessor::release(EngineKind kind) {
    if (kind==EngineKind::compute) dut_.compute_release=1;
    if (kind==EngineKind::copy) dut_.copy_release=1;
    settle();
}

bool RtlCommandProcessor::busy(EngineKind kind) const {
    return kind==EngineKind::compute ? dut_.compute_busy : kind==EngineKind::copy && dut_.copy_busy;
}

void RtlCommandProcessor::count_events() {
    ++stats_.clocks;
    const auto selected=dut_.dispatch_valid;
    const bool active=dut_.slot_valid || dut_.mmio_valid || dut_.fetch_accept || selected;
    if (active) ++stats_.active_clocks;
    if (dut_.fetch_accept) ++stats_.fetched;
    if (selected && !dut_.dispatch_cancel) ++stats_.dispatched;
    if (selected && dut_.dispatch_cancel) ++stats_.cancelled;
    stats_.completed+=static_cast<uint64_t>(std::popcount(static_cast<uint32_t>(dut_.complete_mask)));
    stats_.failed+=static_cast<uint64_t>(std::popcount(static_cast<uint32_t>(dut_.failed_mask)));
}

void RtlCommandProcessor::tick() {
    settle(); count_events(); edge(); clear_event_pins(); dut_.eval();
    stats_.peak_slots=std::max<uint64_t>(stats_.peak_slots,static_cast<uint64_t>(std::popcount(static_cast<uint32_t>(dut_.slot_valid))));
}
}

std::unique_ptr<CommandProcessor> make_rtl_command_processor(uint32_t capacity) {
    return std::make_unique<RtlCommandProcessor>(capacity);
}
}

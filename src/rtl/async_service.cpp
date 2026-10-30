#include "simt/async_service.hpp"
#include "Vsimt_async_service.h"
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlAsyncService final : public AsyncService {
public:
    RtlAsyncService(uint32_t ctas,uint32_t latency);
    ~RtlAsyncService() override { dut_.final(); }
    void offer(const AsyncServiceInputs& inputs) override;
    AsyncServiceState selection() const override;
    AsyncServiceState read() const override;
    bool foreground_ready() const override { return dut_.foreground_ready; }
    void accept_instruction(uint64_t id,uint32_t cta) override;
    void accept_background() override;
    void complete() override { event(dut_.complete,"async_service_completion_phase"); }
    void retry() override { event(dut_.retry,"async_service_retry_phase"); }
    void acknowledge_retirement() override { event(dut_.retire_ack,"async_service_retirement_phase"); }
    void tick() override;
    const AsyncServiceStats& stats() const override { return stats_; }
private:
    Vsimt_async_service dut_;
    AsyncServiceStats stats_;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void clear();
    void event(CData& pin,const char* message);
};
void RtlAsyncService::clear() {
    dut_.dma_valid=0; dut_.retire_requests=0; dut_.table_ready=0;
    dut_.accept=0; dut_.complete=0; dut_.retry=0; dut_.retire_ack=0;
}
RtlAsyncService::RtlAsyncService(uint32_t ctas,uint32_t latency) {
    if (!ctas || ctas>64 || !latency) throw std::out_of_range("invalid_async_service_configuration");
    clear(); dut_.cta_limit=ctas; dut_.latency=latency;
    dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}

// Rejected offers are rolled back; an accepted transfer holds its wires until the next edge.
// 拒绝的输入会撤回；已接受传输保持其连线直到下一时钟边沿。
void RtlAsyncService::offer(const AsyncServiceInputs& inputs) {
    if (dut_.accept) return;
    if (dut_.cta_limit<64 && (inputs.retire_requests>>dut_.cta_limit)) throw std::out_of_range("async_service_retirement_mask");
    dut_.dma_valid=inputs.dma_valid; dut_.dma_id=inputs.dma_id; dut_.dma_cta=inputs.dma_cta;
    dut_.retire_requests=inputs.retire_requests; dut_.table_ready=inputs.table_ready;
    for (uint32_t slot=0; slot<64; ++slot) dut_.retire_ctas[slot]=inputs.retire_ctas[slot];
    dut_.eval();
    if (dut_.error) throw std::runtime_error("async_service_offer_error");
}
AsyncServiceState RtlAsyncService::selection() const {
    return {static_cast<AsyncOwner>(dut_.grant_kind),dut_.grant_id,dut_.grant_cta,dut_.grant_slot,dut_.cursor,0,false,false,false};
}
AsyncServiceState RtlAsyncService::read() const {
    return {static_cast<AsyncOwner>(dut_.owner_kind),dut_.owner_id,dut_.owner_cta,dut_.owner_slot,dut_.cursor,dut_.remaining,
            bool(dut_.service_ready),bool(dut_.retirement_done),bool(dut_.pending)};
}
void RtlAsyncService::event(CData& pin,const char* message) {
    if (pin || dut_.accept) throw std::runtime_error(message);
    pin=1; dut_.eval();
    if (!dut_.error) return;
    pin=0; dut_.eval(); throw std::runtime_error(message);
}
void RtlAsyncService::accept_instruction(uint64_t id,uint32_t cta) {
    if (!foreground_ready()) throw std::runtime_error("async_service_instruction_not_granted");
    dut_.instruction_id=id; dut_.instruction_cta=cta; event(dut_.accept,"async_service_accept_phase");
}
void RtlAsyncService::accept_background() {
    if (dut_.grant_kind<2) throw std::runtime_error("async_service_background_not_granted");
    event(dut_.accept,"async_service_accept_phase");
}
void RtlAsyncService::tick() {
    if (dut_.error) throw std::runtime_error("async_service_protocol_error");
    ++stats_.clocks;
    if (dut_.accepted_kind) ++stats_.accepted[dut_.accepted_kind-1];
    if (dut_.completed_kind) ++stats_.completed[dut_.completed_kind-1];
    stats_.retries+=bool(dut_.retry); stats_.retirement_acks+=bool(dut_.retire_ack);
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear(); dut_.eval();
}
}
std::unique_ptr<AsyncService> make_async_service(uint32_t ctas,uint32_t latency) {
    return std::make_unique<RtlAsyncService>(ctas,latency);
}
}

#include "simt/execution_arbiter.hpp"
#include "Vsimt_execution_arbiter.h"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlExecutionArbiter final : public ExecutionArbiter {
public:
    RtlExecutionArbiter(uint32_t partitions,std::array<uint32_t,3> intervals);
    ~RtlExecutionArbiter() override { dut_.final(); }
    void release_tensor(uint32_t partition) override;
    uint64_t arbitrate(uint64_t requests,uint32_t async_noop,bool async_busy) override;
    bool granted(uint32_t pipe,uint32_t partition) const override;
    void accept(uint32_t pipe,uint32_t partition) override;
    ExecutionArbiterState read() const override;
    const ExecutionArbiterStats& stats() const override { return stats_; }
    void tick() override;
private:
    Vsimt_execution_arbiter dut_;
    ExecutionArbiterStats stats_;
    bool arbitrated_=false;
    void clear();
    void sample();
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    uint64_t bit(uint32_t pipe,uint32_t partition) const;
};
void RtlExecutionArbiter::clear() {
    dut_.requests=0; dut_.accepts=0; dut_.async_noop=0; dut_.async_busy=0;
    dut_.tensor_release=0; arbitrated_=false;
}
RtlExecutionArbiter::RtlExecutionArbiter(uint32_t partitions,std::array<uint32_t,3> intervals) {
    if (!partitions || partitions>8 || !intervals[0] || !intervals[1] || !intervals[2])
        throw std::out_of_range("invalid_execution_arbiter_config");
    clear(); dut_.partition_limit=partitions; dut_.integer_interval=intervals[0];
    dut_.floating_interval=intervals[1]; dut_.special_interval=intervals[2];
    dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}
uint64_t RtlExecutionArbiter::bit(uint32_t pipe,uint32_t partition) const {
    if (pipe>=7 || partition>=dut_.partition_limit) throw std::out_of_range("execution_arbiter_port");
    return uint64_t{1}<<(pipe*8+partition);
}
void RtlExecutionArbiter::release_tensor(uint32_t partition) {
    static_cast<void>(bit(5,partition));
    if (arbitrated_ || dut_.tensor_release) throw std::runtime_error("execution_arbiter_release_phase");
    dut_.tensor_release_owner=partition; dut_.tensor_release=1; dut_.eval();
    if (!dut_.error) return;
    dut_.tensor_release=0; dut_.eval(); throw std::runtime_error("execution_arbiter_invalid_release");
}

// A single arbitration phase freezes request wires until all physical consumers have acknowledged.
// 单次仲裁阶段固定请求连线，直到全部物理接收方完成确认。
uint64_t RtlExecutionArbiter::arbitrate(uint64_t requests,uint32_t async_noop,bool async_busy) {
    if (arbitrated_) throw std::runtime_error("execution_arbiter_already_arbitrated");
    if ((requests>>56) || async_noop>255) throw std::out_of_range("execution_arbiter_request_width");
    dut_.requests=requests; dut_.async_noop=async_noop; dut_.async_busy=async_busy; dut_.eval();
    if (dut_.error) {
        dut_.requests=0; dut_.async_noop=0; dut_.async_busy=0; dut_.eval();
        throw std::runtime_error("execution_arbiter_invalid_requests");
    }
    arbitrated_=true; return dut_.grants;
}
bool RtlExecutionArbiter::granted(uint32_t pipe,uint32_t partition) const {
    return arbitrated_ && ((dut_.grants & ~dut_.accepts) & bit(pipe,partition));
}
void RtlExecutionArbiter::accept(uint32_t pipe,uint32_t partition) {
    if (!granted(pipe,partition)) throw std::runtime_error("execution_arbiter_not_granted");
    dut_.accepts|=bit(pipe,partition); dut_.eval();
    if (dut_.error) throw std::runtime_error("execution_arbiter_accept_rejected");
}
ExecutionArbiterState RtlExecutionArbiter::read() const {
    ExecutionArbiterState state{bool(dut_.tensor_busy),dut_.tensor_owner,dut_.tensor_cursor,dut_.async_cursor,{}};
    for (uint32_t at=0; at<24; ++at) state.cooldowns[at]=dut_.cooldowns[at];
    return state;
}
void RtlExecutionArbiter::sample() {
    for (uint32_t pipe=0; pipe<7; ++pipe) {
        const auto shift=pipe*8;
        const auto events=[shift](uint64_t mask) { return std::popcount(uint32_t((mask>>shift)&255)); };
        stats_.requests[pipe]+=events(dut_.requests); stats_.grants[pipe]+=events(dut_.grants);
        stats_.accepted[pipe]+=events(dut_.accepts); stats_.interval_stalls[pipe]+=events(dut_.interval_stalls);
        stats_.shared_stalls[pipe]+=events(dut_.shared_stalls); stats_.arbitration_stalls[pipe]+=events(dut_.arbitration_stalls);
        stats_.downstream_stalls[pipe]+=events(dut_.grants & ~dut_.accepts);
    }
    stats_.tensor_releases+=bool(dut_.tensor_release);
}
void RtlExecutionArbiter::tick() {
    if (dut_.error) throw std::runtime_error("execution_arbiter_protocol_error");
    ++stats_.clocks; sample();
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear(); dut_.eval();
}
}
std::unique_ptr<ExecutionArbiter> make_execution_arbiter(uint32_t partitions,std::array<uint32_t,3> intervals) {
    return std::make_unique<RtlExecutionArbiter>(partitions,intervals);
}
}

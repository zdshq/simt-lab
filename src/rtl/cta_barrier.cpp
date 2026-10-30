#include "simt/cta_barrier.hpp"
#include "Vsimt_cta_barrier.h"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
// Read an eleven-bit counter even when its packed field crosses a Verilator word boundary.
// 读取十一位计数器，包括跨越 Verilator 字边界的打包字段。
template<class Words> uint32_t counter(const Words& words,uint32_t index) {
    const auto shift=index*11; uint64_t bits=words[shift/32];
    if (shift%32>21) bits|=uint64_t{words[shift/32+1]}<<32;
    return (bits>>(shift%32))&2047;
}
class RtlCtaBarrier final : public CtaBarrierEngine {
public:
    RtlCtaBarrier() { dut_.clk=0; dut_.command_valid=0; dut_.initialize=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval(); }
    ~RtlCtaBarrier() override { dut_.final(); }
    void initialize(uint32_t threads) override;
    bool available() const override { return dut_.ready && !dut_.command_valid; }
    uint32_t execute(const CtaBarrierCommand& command) override;
    CtaBarrierState read() const override;
    void tick() override;
    const CtaBarrierStats& stats() const override { return stats_; }
private:
    Vsimt_cta_barrier dut_;
    CtaBarrierStats stats_;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
};
void RtlCtaBarrier::initialize(uint32_t threads) {
    if (dut_.initialize || dut_.command_valid) throw std::runtime_error("cta_barrier_initialize_phase");
    if (!threads || threads>1024) throw std::out_of_range("invalid_cta_threads");
    dut_.initial_threads=threads; dut_.initialize=1; dut_.eval();
}

// The single command remains on its pins until tick; rejection withdraws it without changing storage.
// 单个命令保留在引脚上直到 tick；拒绝时撤销命令，不改变存储。
uint32_t RtlCtaBarrier::execute(const CtaBarrierCommand& c) {
    if (!available()) throw std::runtime_error("cta_barrier_command_phase");
    if (c.warp>=32 || c.barrier>=64) throw std::out_of_range("cta_barrier_descriptor");
    dut_.warp=c.warp; dut_.mask=c.mask; dut_.barrier=c.barrier;
    dut_.requested_count=c.count; dut_.is_exit=c.exit; dut_.command_valid=1; dut_.eval();
    if (dut_.accept) return dut_.release_mask;
    const auto error=dut_.error; dut_.command_valid=0; dut_.eval();
    static constexpr std::array messages{"cta_barrier_rejected","invalid_cta_threads","cta_barrier_uninitialized",
        "cta_barrier_id_out_of_range","invalid_barrier_count","barrier_count_mismatch",
        "duplicate_barrier_arrival","barrier_arrival_overflow","barrier_inactive_lane"};
    throw std::runtime_error(error<messages.size() ? messages[error] : "unknown_cta_barrier_error");
}

// Snapshots inspect the physical table; callers cannot feed an arrival bitmap back into it.
// 快照读取物理表；调用方不能把到达位图回填为表的输入。
CtaBarrierState RtlCtaBarrier::read() const {
    CtaBarrierState result; result.threads=dut_.observed_threads; result.live_count=dut_.observed_live_count;
    for (uint32_t word=0; word<32; ++word) result.live[word]=dut_.observed_live[word];
    for (uint32_t id=0; id<16; ++id) {
        auto& r=result.records[id]; r.active=(dut_.active_mask>>id)&1;
        r.count=counter(dut_.counts,id); r.arrived=counter(dut_.arrived_counts,id); r.expected=counter(dut_.expected_counts,id);
        r.generation=uint64_t{dut_.generations[id*2]}|(uint64_t{dut_.generations[id*2+1]}<<32);
        for (uint32_t word=0; word<32; ++word) r.members[word]=dut_.arrivals[id*32+word];
    }
    return result;
}
void RtlCtaBarrier::tick() {
    ++stats_.clocks; stats_.initializations+=bool(dut_.initialize);
    if (dut_.accept) {
        ++stats_.commands; stats_.completions+=std::popcount(uint32_t{dut_.release_mask});
        if (dut_.is_exit) { ++stats_.exits; stats_.exit_lanes+=dut_.exit_lanes; }
        else { ++stats_.arrivals; stats_.arrival_lanes+=dut_.arrival_lanes; }
    }
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    dut_.initialize=0; dut_.command_valid=0; dut_.eval();
}
}
std::unique_ptr<CtaBarrierEngine> make_cta_barrier() { return std::make_unique<RtlCtaBarrier>(); }
}

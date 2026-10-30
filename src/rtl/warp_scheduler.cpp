#include "simt/warp_scheduler.hpp"
#include "Vsimt_warp_scheduler.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
template<class Words> uint32_t count(const Words& words,uint32_t slot) {
    uint32_t value=0;
    for (uint32_t bit=0; bit<6; ++bit) value|=((words[(slot*6+bit)/32]>>((slot*6+bit)%32))&1)<<bit;
    return value;
}
template<class Words> void set_count(Words& words,uint32_t slot,uint32_t value) {
    for (uint32_t bit=0; bit<6; ++bit) {
        const auto index=slot*6+bit, mask=uint32_t{1}<<(index%32);
        words[index/32]=(words[index/32]&~mask)|(((value>>bit)&1)<<(index%32));
    }
}
class RtlWarpScheduler final : public WarpScheduler {
public:
    explicit RtlWarpScheduler(uint32_t slots);
    ~RtlWarpScheduler() override { dut_.final(); }
    void allocate(uint32_t slot) override;
    void release(uint32_t slot) override;
    void retire(uint32_t slot,bool boundary,bool memory) override;
    std::optional<uint32_t> probe(uint32_t runnable,uint32_t policy,bool collector,bool completion) override;
    bool decide(bool guard,bool dependencies,bool boundary,bool memory) override;
    SchedulerState read() const override;
    void tick() override;
    const SchedulerStats& stats() const override { return stats_; }
private:
    Vsimt_warp_scheduler dut_;
    SchedulerStats stats_;
    void clear();
    void check_slot(uint32_t slot) const;
    void check_event_phase() const;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void count_probe();
};
void RtlWarpScheduler::clear() {
    dut_.allocate_mask=0; dut_.release_mask=0; dut_.runnable=0; dut_.retire_boundaries=0; dut_.retire_memories=0;
    dut_.decision_valid=0; dut_.collector_ready=0; dut_.completion_ready=0;
    for (auto& word : dut_.retire_counts) word=0;
}
RtlWarpScheduler::RtlWarpScheduler(uint32_t slots) {
    if (!slots || slots>32) throw std::out_of_range("invalid_scheduler_slots");
    clear(); dut_.slot_limit=slots; dut_.policy=0; dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}
void RtlWarpScheduler::check_slot(uint32_t slot) const {
    if (slot>=dut_.slot_limit) throw std::out_of_range("scheduler_slot_out_of_range");
}
void RtlWarpScheduler::check_event_phase() const {
    if (dut_.decision_valid) throw std::runtime_error("scheduler_event_after_decision");
}
void RtlWarpScheduler::allocate(uint32_t slot) {
    check_event_phase(); check_slot(slot); const auto mask=uint32_t{1}<<slot;
    if (dut_.allocate_mask&mask) throw std::runtime_error("scheduler_duplicate_allocation");
    dut_.allocate_mask|=mask; dut_.eval();
    if (!dut_.error) return;
    dut_.allocate_mask&=~mask; dut_.eval(); throw std::runtime_error("scheduler_invalid_allocation");
}
void RtlWarpScheduler::release(uint32_t slot) {
    check_event_phase(); check_slot(slot); const auto mask=uint32_t{1}<<slot;
    if (dut_.release_mask&mask) throw std::runtime_error("scheduler_duplicate_release");
    dut_.release_mask|=mask; dut_.eval();
    if (!dut_.error) return;
    dut_.release_mask&=~mask; dut_.eval(); throw std::runtime_error("scheduler_invalid_release");
}

// Retirement events accumulate only on input pins; visible counts are the circuit's same-cycle bypass.
// 退休事件仅累积到输入引脚；可见计数来自电路的同拍旁路。
void RtlWarpScheduler::retire(uint32_t slot,bool boundary,bool memory) {
    check_event_phase(); check_slot(slot); const auto old=count(dut_.retire_counts,slot), mask=uint32_t{1}<<slot;
    if (old==32 || (boundary && (dut_.retire_boundaries&mask)) || (memory && (dut_.retire_memories&mask)))
        throw std::runtime_error("scheduler_retirement_capacity");
    const auto old_boundary=dut_.retire_boundaries, old_memory=dut_.retire_memories;
    set_count(dut_.retire_counts,slot,old+1);
    if (boundary) dut_.retire_boundaries|=mask;
    if (memory) dut_.retire_memories|=mask;
    dut_.eval(); if (!dut_.error) return;
    set_count(dut_.retire_counts,slot,old); dut_.retire_boundaries=old_boundary; dut_.retire_memories=old_memory;
    dut_.eval(); throw std::runtime_error("scheduler_invalid_retirement");
}
std::optional<uint32_t> RtlWarpScheduler::probe(uint32_t runnable,uint32_t policy,bool collector,bool completion) {
    if (dut_.decision_valid) throw std::runtime_error("scheduler_probe_after_decision");
    if (policy>2 || (dut_.slot_limit<32 && (runnable>>dut_.slot_limit))) throw std::out_of_range("invalid_scheduler_probe");
    dut_.runnable=runnable; dut_.policy=policy; dut_.collector_ready=collector; dut_.completion_ready=completion; dut_.eval();
    if (!dut_.candidate_valid) return {};
    return dut_.candidate;
}
bool RtlWarpScheduler::decide(bool guard,bool dependencies,bool boundary,bool memory) {
    if (dut_.decision_valid || !dut_.candidate_valid) throw std::runtime_error("scheduler_decision_phase");
    dut_.guard_ready=guard; dut_.dependencies_ready=dependencies; dut_.instruction_boundary=boundary;
    dut_.instruction_memory=memory; dut_.decision_valid=1; dut_.eval(); return dut_.issue_accept;
}
SchedulerState RtlWarpScheduler::read() const {
    SchedulerState s{dut_.live_mask,dut_.boundary_mask,dut_.memory_mask,dut_.cursor,dut_.skipped,{}};
    for (uint32_t slot=0; slot<32; ++slot) s.inflight[slot]=count(dut_.inflight_counts,slot);
    return s;
}
void RtlWarpScheduler::count_probe() {
    if (!dut_.candidate_valid || !dut_.decision_valid) return;
    ++stats_.probes; stats_.issued+=bool(dut_.issue_accept);
    switch (dut_.rejection) {
    case 1: ++stats_.guard_stalls; break;
    case 2: ++stats_.boundary_stalls; break;
    case 3: ++stats_.memory_stalls; break;
    case 4: ++stats_.dependency_stalls; break;
    case 5: ++stats_.count_stalls; break;
    default: break;
    }
}
void RtlWarpScheduler::tick() {
    if (dut_.error) throw std::runtime_error("scheduler_protocol_error");
    ++stats_.clocks; count_probe(); stats_.admitted+=std::popcount(dut_.allocate_mask); stats_.released+=std::popcount(dut_.release_mask);
    for (uint32_t slot=0; slot<32; ++slot) stats_.retired+=count(dut_.retire_counts,slot);
    stats_.collector_stalls+=bool(dut_.collector_stall); stats_.completion_stalls+=bool(dut_.completion_stall);
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear(); dut_.eval(); stats_.peak_inflight=std::max(stats_.peak_inflight,uint64_t{dut_.total_inflight});
}
}
std::unique_ptr<WarpScheduler> make_warp_scheduler(uint32_t slots) { return std::make_unique<RtlWarpScheduler>(slots); }
}

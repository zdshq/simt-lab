#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
// Each frontend provides its own group wires; only the selected warp probes the shared predicate port.
// 每个前端提供自身线程组连线；仅被选中的 warp 查询共享谓词端口。
bool TimingMachine::issue_partition_rtl(uint32_t sm_id,uint32_t part) {
    auto& sm=*sms_[sm_id]; auto& partition=sm.partitions[part]; uint32_t runnable=0;
    std::array<std::optional<Group>,32> groups;
    for (uint32_t slot=part; slot<sm.owners.size(); slot+=sm.partitions.size()) {
        if (!sm.owners[slot]) continue;
        const auto owner=*sm.owners[slot]; const auto local=slot/sm.partitions.size();
        groups[local]=select_warp_group(owner.cta,owner.warp);
        if (groups[local]) runnable|=uint32_t{1}<<local;
    }
    const auto chosen=partition.scheduler->probe(runnable,static_cast<uint32_t>(machine_.config_.warp_policy),
        partition.registers->collector()->available(),partition.registers->writeback()->available());
    if (!chosen) return false;
    const auto owner=*sm.owners[*chosen*sm.partitions.size()+part]; const auto group=*groups[*chosen];
    try { return issue_scheduled_group(owner.cta,owner.warp,group); }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string(e.what())+timing_location(owner.cta,sm_id,owner.warp,group.pc));
    }
}

// Raw readiness enters the scheduler; it applies its own in-flight, boundary and memory-order state.
// 原始就绪信号进入调度器；由调度器应用自身在途、边界和访存顺序状态。
bool TimingMachine::issue_scheduled_group(uint32_t cta_id,uint32_t wi,Group group) {
    const auto sm=machine_.ctas_[cta_id].sm_id; auto& timed=warps_[cta_id][wi];
    auto& scheduler=*sms_[sm]->partitions[timed.slot%sms_[sm]->partitions.size()].scheduler;
    if (group.pc>=machine_.module_.code.size()) throw std::runtime_error("pc_out_of_range");
    const auto& i=machine_.module_.code[group.pc];
    if (!timed.scoreboard->predicate_ready(i.guard&7,group.mask)) return scheduler.decide(false,false,false,false);
    const auto predicates=probe_predicates(cta_id,wi,i,group);
    auto footprint=instruction_footprint(i,predicates.participating);
    if (!scheduler.decide(true,dependencies(cta_id,wi,group,footprint),footprint.boundary,footprint.pipe==Pipe::memory)) return false;
    enqueue(cta_id,wi,group,predicates.participating,std::move(footprint),predicates.source); return true;
}

uint32_t TimingMachine::warp_inflight(uint32_t cta,uint32_t wi) const {
    const auto& sm=*sms_[machine_.ctas_[cta].sm_id]; const auto& timed=warps_[cta][wi];
    const auto& scheduler=sm.partitions[timed.slot%sm.partitions.size()].scheduler;
    return scheduler ? scheduler->read().inflight[timed.slot/sm.partitions.size()] : timed.inflight;
}

// Retirement waits for the physical release edge before resources can be assigned to a new CTA.
// 退休等待物理释放边沿完成，之后才能将资源分配给新 CTA。
bool TimingMachine::release_scheduler_cta(const Cta& cta) {
    auto& sm=*sms_[cta.sm_id];
    if (!sm.partitions.front().scheduler || scheduler_releasing_[cta.block_id]) return true;
    for (const auto& warp : warps_[cta.block_id])
        sm.partitions[warp.slot%sm.partitions.size()].scheduler->release(warp.slot/sm.partitions.size());
    scheduler_releasing_[cta.block_id]=true; return false;
}
}

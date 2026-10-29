#include "simt/timing.hpp"
#include "simt/control.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt {
TimedSm::TimedSm(const hw::PipelineConfig& config) : resources(config), owners(config.max_warps) {
    for (uint32_t part = 0; part < config.partitions; ++part) partitions.emplace_back(config.register_banks);
}

TimingMachine::TimingMachine(Machine& machine) : machine_(machine) {
    const auto& config = machine_.config_;
    if (!config.max_cycles) throw std::runtime_error("invalid_cycle_limit");
    hw::SmResources probe(config.pipeline);
    if (!probe.acquire(0, config.threads, config.cta_shared_bytes))
        throw std::runtime_error("kernel_exceeds_sm_resources");
    hw::HierarchyConfig memory_config; memory_config.sms = config.sms;
    memory_ = std::make_shared<hw::MemoryHierarchy>(*machine_.global_, memory_config);
    for (uint32_t sm = 0; sm < config.sms; ++sm) sms_.push_back(std::make_unique<TimedSm>(config.pipeline));
    warps_.resize(config.blocks);
}

// Admission rotates across SMs before filling additional CTA slots on any one SM.
// 准入先在 SM 之间轮转，再填充某个 SM 的额外 CTA 槽位。
bool TimingMachine::admit() {
    bool any = false, progress = true;
    const auto& config = machine_.config_;
    while (progress && machine_.next_block_ < config.blocks) {
        progress = false;
        for (uint32_t sm = 0; sm < sms_.size() && machine_.next_block_ < config.blocks; ++sm) {
            const auto block = machine_.next_block_;
            const auto residency = sms_[sm]->resources.acquire(block, config.threads, config.cta_shared_bytes);
            if (!residency) continue;
            ++machine_.next_block_; place(block, sm, *residency);
            progress = true; any = true;
        }
    }
    return any;
}

void TimingMachine::place(uint32_t block, uint32_t sm, const hw::Residency& residency) {
    machine_.initialize_cta(block, sm);
    warps_[block].resize(residency.slots.size());
    for (uint32_t warp = 0; warp < residency.slots.size(); ++warp) {
        const auto slot = residency.slots[warp];
        warps_[block][warp].slot = slot;
        sms_[sm]->owners[slot] = WarpOwner{block, warp};
    }
    stats_.peak_ctas = std::max(stats_.peak_ctas, uint64_t{sms_[sm]->resources.ctas()});
    Inflight event; event.sm = sm; event.cta = block; trace(event, "admit");
}

// EXIT does not free occupancy until every issued instruction has retired.
// EXIT 不会立即释放占用，必须等待所有已发射指令退休。
void TimingMachine::retire_ctas() {
    for (auto& cta : machine_.ctas_) {
        if (cta.completed || cta.warps.empty()) continue;
        bool done = true;
        for (uint32_t wi = 0; wi < cta.warps.size(); ++wi) {
            done &= warps_[cta.block_id][wi].inflight == 0;
            for (auto state : cta.warps[wi].state) done &= state == LaneState::exited;
        }
        if (!done) continue;
        auto& sm = *sms_[cta.sm_id];
        for (const auto& warp : warps_[cta.block_id]) sm.owners[warp.slot].reset();
        sm.resources.release(cta.block_id);
        cta.completed = true; ++machine_.completed_;
        Inflight event; event.cta = cta.block_id; event.sm = cta.sm_id; trace(event, "cta_retire");
    }
}

// Every partition can issue one ready warp group per clock from its assigned physical slots.
// 每个分区每拍可从所属物理槽位发射一个就绪 warp 线程组。
bool TimingMachine::issue_partition(uint32_t sm_id, uint32_t part) {
    auto& sm = *sms_[sm_id];
    auto& partition = sm.partitions[part];
    for (uint32_t offset = 0; offset < sm.owners.size(); ++offset) {
        auto slot = (partition.cursor + offset) % sm.owners.size();
        if (machine_.config_.warp_policy == Policy::min_pc) slot = offset;
        if (machine_.config_.warp_policy == Policy::max_pc) slot = sm.owners.size() - 1 - offset;
        if (slot % sm.partitions.size() != part || !sm.owners[slot]) continue;
        const auto owner = *sm.owners[slot];
        if (!try_issue(owner.cta, owner.warp)) continue;
        partition.cursor = static_cast<uint32_t>((slot + 1) % sm.owners.size());
        return true;
    }
    return false;
}

// Pending execution or memory transactions distinguish latency stalls from genuine deadlock.
// 待完成执行或内存事务用于区分延迟等待与真正死锁。
bool TimingMachine::advance() {
    if (machine_.completed_ == machine_.config_.blocks) return true;
    if (cycle_ >= machine_.config_.max_cycles) throw std::runtime_error("cycle_limit_exceeded");
    admit(); ++cycle_;
    memory_->tick();
    for (auto& sm : sms_) { sm->shared.tick(); sm->constant.tick(); }
    retire_results(); collect_operands(); launch_ready();
    bool issued = false;
    for (uint32_t sm = 0; sm < sms_.size(); ++sm)
        for (uint32_t part = 0; part < sms_[sm]->partitions.size(); ++part) issued |= issue_partition(sm, part);
    retire_ctas();
    if (!issued && inflight_.empty() && machine_.completed_ != machine_.config_.blocks && !admit())
        throw std::runtime_error(machine_.deadlock_message());
    return machine_.completed_ == machine_.config_.blocks;
}
}

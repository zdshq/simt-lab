#include "simt/timing.hpp"
#include "simt/control.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt {
namespace {
void validate_async_config(const hw::AsyncConfig& config) {
    if (!config.commands || config.commands > 64 || !config.fragments || config.fragments > 64 ||
        !config.chunk_bytes || config.chunk_bytes > 128 || !config.barrier_slots || config.barrier_slots > 1024 ||
        !config.barrier_latency) throw std::runtime_error("invalid_async_pipeline_config");
}
}

TimedSm::TimedSm(const hw::PipelineConfig& config, const hw::TensorConfig& tensor, const hw::AtomicConfig& atomic)
    : resources(config), owners(config.max_warps),
      shared({65536, 32, 2, 8, atomic.latency}),
      tmem({131072, tensor.tmem_banks, tensor.tmem_latency, 8}), array(tensor) {
    for (uint32_t part = 0; part < config.partitions; ++part) partitions.emplace_back(config.register_banks);
}

TimingMachine::TimingMachine(Machine& machine) : machine_(machine), physical_(*machine.global_) {
    const auto& config = machine_.config_;
    if (!config.max_cycles) throw std::runtime_error("invalid_cycle_limit");
    validate_async_config(config.async);
    hw::SmResources probe(config.pipeline);
    if (!probe.acquire(0, config.threads, config.cta_shared_bytes))
        throw std::runtime_error("kernel_exceeds_sm_resources");
    hw::HierarchyConfig memory_config; memory_config.sms = config.sms; memory_config.atomic = config.atomic;
    memory_ = std::make_shared<hw::MemoryHierarchy>(physical_, memory_config);
    for (uint32_t sm = 0; sm < config.sms; ++sm) sms_.push_back(std::make_unique<TimedSm>(config.pipeline, config.tensor, config.atomic));
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
        auto threads = std::span<Memory>(machine_.ctas_[block].local).subspan(warp * lanes);
        physical_.attach(block * 32 + warp, threads.first(std::min<size_t>(threads.size(), lanes)));
        const auto slot = residency.slots[warp];
        warps_[block][warp].slot = slot;
        sms_[sm]->owners[slot] = WarpOwner{block, warp};
    }
    stats_.peak_ctas = std::max(stats_.peak_ctas, uint64_t{sms_[sm]->resources.ctas()});
    Inflight event; event.sm = sm; event.cta = block; trace(event, "admit");
}

// Exited lanes still own their CTA while issued instructions or DMA commands are outstanding.
// lane 退出后，只要已发射指令或 DMA 命令仍未完成，就继续持有 CTA。
bool TimingMachine::cta_drained(const Cta& cta) const {
    if (cta.completed || cta.warps.empty() || pending_copy(cta.block_id)) return false;
    for (uint32_t wi = 0; wi < cta.warps.size(); ++wi) {
        if (warps_[cta.block_id][wi].inflight) return false;
        for (auto state : cta.warps[wi].state) if (state != LaneState::exited) return false;
    }
    return true;
}

// Resource credits return only after payload completion, including transaction-table entries.
// 载荷完成后才归还资源额度，包括事务表条目。
void TimingMachine::retire_ctas() {
    for (auto& cta : machine_.ctas_) {
        if (!cta_drained(cta)) continue;
        for (const auto& [address, barrier] : cta.transactions) {
            static_cast<void>(address);
            if (barrier.bytes || barrier.issued) throw std::runtime_error("incomplete_async_transaction");
        }
        auto& sm = *sms_[cta.sm_id];
        for (const auto& warp : warps_[cta.block_id]) sm.owners[warp.slot].reset();
        sm.resources.release(cta.block_id);
        sm.allocated_columns -= sm.tensor_columns[cta.block_id];
        sm.tensor_columns.erase(cta.block_id);
        sm.barrier_entries -= static_cast<uint32_t>(cta.transactions.size());
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
    for (auto& sm : sms_) { sm->shared.tick(); sm->constant.tick(); sm->tmem.tick(); sm->array.tick(); }
    progress_async_units();
    retire_results(); collect_operands(); launch_ready();
    bool issued = false;
    for (uint32_t sm = 0; sm < sms_.size(); ++sm)
        for (uint32_t part = 0; part < sms_[sm]->partitions.size(); ++part) issued |= issue_partition(sm, part);
    retire_ctas();
    if (!issued && inflight_.empty() && !background_work() && machine_.completed_ != machine_.config_.blocks && !admit())
        throw std::runtime_error(machine_.deadlock_message());
    return machine_.completed_ == machine_.config_.blocks;
}
}

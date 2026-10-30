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
      shared(hw::make_shared_engine({65536, 32, 2, 8, atomic.latency},config.collector_rtl)),
      tmem({131072, tensor.tmem_banks, tensor.tmem_latency, 8}), array(hw::make_tensor_engine(tensor)) {
    if (config.collector_rtl) execution_arbiter=hw::make_execution_arbiter(config.partitions,
        {config.integer_interval,config.float_interval,config.special_interval});
    for (uint32_t part = 0; part < config.partitions; ++part) {
        partitions.emplace_back(config.register_banks);
        if (config.register_rtl) partitions.back().registers = hw::make_register_file(config.partitions, config.register_banks, config.collector_rtl ? config.collectors : 0);
        if (config.collector_rtl) partitions.back().scheduler = hw::make_warp_scheduler(32/config.partitions);
        if (config.collector_rtl) partitions.back().decoder = hw::make_instruction_decoder();
    }
    scoreboards.resize(config.max_warps); controls.resize(config.max_warps);
    if (config.collector_rtl) { cta_tables.resize(config.max_ctas); cta_owners.resize(config.max_ctas); }
}

TimingMachine::TimingMachine(Machine& machine, std::shared_ptr<hw::HardwareContext> hardware)
    : machine_(machine), hardware_(std::move(hardware)), external_clock_(bool(hardware_)) {
    const auto& config = machine_.config_;
    if (!config.max_cycles) throw std::runtime_error("invalid_cycle_limit");
    validate_async_config(config.async);
    if (config.pipeline.collector_rtl && machine.module_.image.size()!=machine.module_.code.size())
        throw std::runtime_error("instruction_image_missing");
    if (config.pipeline.collector_rtl && config.pipeline.shared_bytes>65536)
        throw std::runtime_error("shared_rtl_capacity_exceeded");
    hw::SmResources probe(config.pipeline);
    if (config.pipeline.collector_rtl && config.pipeline.float_latency < 4)
        throw std::runtime_error("floating_rtl_requires_latency_at_least_4");
    if (!probe.acquire(0, config.threads, config.cta_shared_bytes))
        throw std::runtime_error("kernel_exceeds_sm_resources");
    hw::HierarchyConfig memory_config; memory_config.sms = config.sms; memory_config.atomic = config.atomic;
    memory_config.cache_rtl = config.cache_rtl;
    if (!hardware_) hardware_ = std::make_shared<hw::HardwareContext>(*machine.global_, memory_config);
    if (hardware_->sms() != config.sms) throw std::runtime_error("hardware_sm_count_mismatch");
    if ((hardware_->memory.l2_cache().name() == "rtl") != config.cache_rtl)
        throw std::runtime_error("hardware_cache_backend_mismatch");
    memory_ = &hardware_->memory;
    owner_ = hardware_->new_owner(); local_base_ = hardware_->reserve_local(uint64_t{config.blocks} * 32);
    started_ = cycle_ = hardware_->cycle();
    if (config.pipeline.collector_rtl)
        instruction_image_=std::make_unique<hw::InstructionMapping>(hardware_->instructions,machine.module_.image);
    for (uint32_t sm = 0; sm < config.sms; ++sm) {
        sms_.push_back(std::make_unique<TimedSm>(config.pipeline, config.tensor, config.atomic));
        if (instruction_image_) sms_.back()->instruction_fetch=hw::make_instruction_fetch(*memory_,*instruction_image_,sm,config.pipeline.partitions,owner_);
        if (config.pipeline.collector_rtl) sms_.back()->transactions = hw::make_transaction_table(config.async.barrier_slots);
        if (config.pipeline.collector_rtl) sms_.back()->dma = hw::make_dma_engine(config.async.commands, config.async.fragments, config.async.chunk_bytes);
        if (config.pipeline.collector_rtl) sms_.back()->async_service=hw::make_async_service(config.pipeline.max_ctas,config.async.barrier_latency);
    }
    warps_.resize(config.blocks); cta_slots_.resize(config.blocks); scheduler_releasing_.resize(config.blocks);
}

TimingMachine::~TimingMachine() { hardware_->physical.detach(local_base_, uint64_t{machine_.config_.blocks} * 32); }

std::optional<hw::Ticket> TimingMachine::submit_memory(uint32_t sm, const hw::WarpAccess& access) {
    return memory_->submit(sm, access, owner_);
}

// A stopped kernel retains local storage until every accepted global/local/atomic request acknowledges.
// 已停止 kernel 保留 local 存储，直到全部已接受 global/local/原子请求确认。
bool TimingMachine::drain() {
    if (!external_clock_) hardware_->tick();
    cycle_ = hardware_->cycle();
    return memory_->drain_owner(owner_);
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
    admit_cta_barrier(block, sm);
    const auto shared_size=machine_.config_.cta_shared_bytes;
    sms_[sm]->shared->attach(machine_.ctas_[block].shared,cta_slots_[block]*((shared_size+127)/128*128),shared_size);
    warps_[block].resize(residency.slots.size());
    for (uint32_t warp = 0; warp < residency.slots.size(); ++warp) {
        auto threads = std::span<Memory>(machine_.ctas_[block].local).subspan(warp * lanes);
        hardware_->physical.attach(local_base_ + block * 32 + warp, threads.first(std::min<size_t>(threads.size(), lanes)));
        const auto slot = residency.slots[warp];
        if (machine_.config_.pipeline.collector_rtl) {
            auto& control = sms_[sm]->controls[slot];
            if (!control) control = hw::make_warp_frontend();
            control->initialize(machine_.ctas_[block].warps[warp].valid_mask);
        }
        auto& file = sms_[sm]->partitions[slot % sms_[sm]->partitions.size()].registers;
        if (file) file->clear(slot / sms_[sm]->partitions.size());
        auto& scheduler=sms_[sm]->partitions[slot % sms_[sm]->partitions.size()].scheduler;
        if (scheduler) scheduler->allocate(slot / sms_[sm]->partitions.size());
        auto& board = sms_[sm]->scoreboards[slot];
        if (!board) board = hw::make_scoreboard_engine(machine_.config_.pipeline.scoreboard_rtl);
        warps_[block][warp].slot = slot;
        warps_[block][warp].scoreboard = board.get();
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
        if (warp_inflight(cta.block_id,wi)) return false;
        for (auto state : cta.warps[wi].state) if (state != LaneState::exited) return false;
    }
    return true;
}

// Resource credits return only after payload completion, including transaction-table entries.
// 载荷完成后才归还资源额度，包括事务表条目。
void TimingMachine::retire_ctas() {
    for (uint32_t sm=0; sm<sms_.size(); ++sm) if (sms_[sm]->async_service) schedule_async_background(sm);
    for (auto& cta : machine_.ctas_) {
        if (!cta_drained(cta)) continue;
        if (!scheduler_releasing_[cta.block_id] && !retire_transaction_cta(cta)) continue;
        if (!release_scheduler_cta(cta)) continue;
        auto& sm = *sms_[cta.sm_id];
        if (!sm.transactions) for (const auto& [address, barrier] : cta.transactions) {
            static_cast<void>(address);
            if (barrier.bytes || barrier.issued) throw std::runtime_error("incomplete_async_transaction");
        }
        for (const auto& warp : warps_[cta.block_id]) sm.owners[warp.slot].reset();
        sm.shared->detach(cta.shared); sm.resources.release(cta.block_id);
        if (!sm.cta_owners.empty()) sm.cta_owners[cta_slots_[cta.block_id]].reset();
        sm.allocated_columns -= sm.tensor_columns[cta.block_id];
        sm.tensor_columns.erase(cta.block_id);
        sm.barrier_entries -= static_cast<uint32_t>(cta.transactions.size());
        cta.completed = true; scheduler_releasing_[cta.block_id]=false; ++machine_.completed_;
        Inflight event; event.cta = cta.block_id; event.sm = cta.sm_id; trace(event, "cta_retire");
    }
}

// Every partition can issue one ready warp group per clock from its assigned physical slots.
// 每个分区每拍可从所属物理槽位发射一个就绪 warp 线程组。
bool TimingMachine::issue_partition(uint32_t sm_id, uint32_t part) {
    auto& sm = *sms_[sm_id];
    auto& partition = sm.partitions[part];
    if (partition.scheduler) return issue_partition_rtl(sm_id,part);
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
    if (cycle_ - started_ >= machine_.config_.max_cycles) throw std::runtime_error("cycle_limit_exceeded");
    if (external_clock_) {
        if (hardware_->cycle() <= cycle_) throw std::runtime_error("hardware_clock_not_advanced");
        cycle_ = hardware_->cycle();
    }
    admit();
    if (!external_clock_) { hardware_->tick(); cycle_ = hardware_->cycle(); }
    for (auto& sm : sms_) { sm->shared->tick(); sm->constant.tick(); sm->tmem.tick(); sm->array->tick(); }
    progress_async_units();
    retire_results(); collect_operands(); launch_ready();
    bool issued = false;
    for (uint32_t sm = 0; sm < sms_.size(); ++sm)
        for (uint32_t part = 0; part < sms_[sm]->partitions.size(); ++part) issued |= issue_partition(sm, part);
    // Physical scoreboard slots persist across CTA reuse and commit once after the cycle's events.
    // 物理 scoreboard 槽在 CTA 复用时保持实例，每拍事件结束后只提交一次。
    for (auto& sm : sms_) {
        for (auto& board : sm->scoreboards) if (board) board->tick();
        for (auto& control : sm->controls) if (control) control->tick();
        for (auto& table : sm->cta_tables) if (table) table->tick();
        if (sm->transactions) sm->transactions->tick();
        if (sm->dma) sm->dma->tick();
        if (sm->async_service) sm->async_service->tick();
        if (sm->execution_arbiter) sm->execution_arbiter->tick();
        if (sm->instruction_fetch) sm->instruction_fetch->tick();
        for (auto& partition : sm->partitions) if (partition.scheduler) partition.scheduler->tick();
        for (auto& partition : sm->partitions) if (partition.decoder) partition.decoder->tick();
        for (auto& partition : sm->partitions) if (partition.registers) partition.registers->tick();
    }
    retire_ctas();
    if (!issued && inflight_.empty() && !background_work() && machine_.completed_ != machine_.config_.blocks && !admit())
        throw std::runtime_error(machine_.deadlock_message());
    return machine_.completed_ == machine_.config_.blocks;
}
}

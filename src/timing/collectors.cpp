#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
// The software mapping retains instruction identities, never a shadow of collector readiness or data.
// 软件映射只保留指令身份，不保存收集器就绪或数据的影子状态。
void TimingMachine::reserve_collector(Inflight& entry) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    auto* collector = partition.registers ? partition.registers->collector() : nullptr;
    if (!collector) { entry.unread = entry.footprint.reads; return; }
    const auto warp = warps_[entry.cta][entry.warp].slot / sms_[entry.sm]->partitions.size();
    entry.collector = collector->allocate(warp, entry.footprint.reads, static_cast<uint32_t>(entry.footprint.pipe));
    if (partition.collector_owners[*entry.collector]) throw std::runtime_error("collector_slot_already_owned");
    partition.collector_owners[*entry.collector] = entry.id;
}

// Read grants come from hardware age arbitration; data goes directly from RF wires to collector latches.
// 读授权来自硬件年龄仲裁；数据从寄存器堆连线直接进入收集器锁存器。
void TimingMachine::collect_rtl_operands() {
    for (auto& sm : sms_) for (auto& partition : sm->partitions) {
        auto* collector = partition.registers ? partition.registers->collector() : nullptr;
        if (!collector) continue;
        const auto before = collector->collector_stats().bank_stalls;
        for (const auto& grant : collector->collect()) {
            const auto owner = partition.collector_owners[grant.entry];
            if (!owner) throw std::runtime_error("collector_grant_without_owner");
            auto& entry = *inflight_.at(*owner);
            warps_[entry.cta][entry.warp].scoreboard->collect_read(grant.reg, grant.mask);
            trace(entry, "operand_read");
        }
        stats_.register_stalls += collector->collector_stats().bank_stalls-before;
        for (uint32_t index = 0; index < partition.collector_owners.size(); ++index)
            if (partition.collector_owners[index] && collector->ready(index))
                inflight_.at(*partition.collector_owners[index])->stage = Stage::ready;
    }
}

// External unit eligibility is separate from the collector's hardware age and port selection.
// 外部单元准入条件与收集器的硬件年龄及端口选择分离。
bool TimingMachine::async_dispatch_eligible(const Inflight& entry) const {
    if (entry.footprint.pipe != Pipe::async || !entry.mask) return true;
    const auto& sm = *sms_[entry.sm];
    const bool copy_ready=sm.dma ? sm.dma->available() : sm.copies.size()<machine_.config_.async.commands;
    return !sm.barrier_job && (entry.instruction.op != Op::CP_ASYNC ||
                              copy_ready);
}

void TimingMachine::dispatch_collector_operands() {
    for (auto& sm : sms_) for (auto& partition : sm->partitions) {
        auto* collector = partition.registers ? partition.registers->collector() : nullptr;
        if (!collector) continue;
        uint32_t eligible = 0;
        for (uint32_t index = 0; index < partition.collector_owners.size(); ++index)
            if (partition.collector_owners[index] && async_dispatch_eligible(*inflight_.at(*partition.collector_owners[index])))
                eligible |= 1u << index;
        collector->dispatch(eligible);
    }
}

// The C++ execution latch receives only completed RTL operands after execution resources accept the work.
// 只有执行资源接受工作后，C++ 执行锁存器才接收已完成的 RTL 操作数。
void TimingMachine::take_collector(Inflight& entry) {
    if (!entry.collector) return;
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    for (const auto& source : partition.registers->collector()->take(*entry.collector))
        entry.operands.r[source.reg] = source.values;
    release_collector_owner(entry);
}

void TimingMachine::release_collector_owner(Inflight& entry) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    partition.collector_owners[*entry.collector].reset();
    entry.collector.reset();
}
}

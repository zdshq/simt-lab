#include "simt/timing.hpp"

namespace simt {
// Candidate wires come from each collector; the shared arbiter chooses partitions without host priority sorting.
// 候选连线来自各收集器；共享仲裁器选择分区，不由宿主排序优先级。
void TimingMachine::arbitrate_execution() {
    for (auto& sm : sms_) {
        if (!sm->execution_arbiter) continue;
        uint64_t requests=0; uint32_t noop=0;
        for (uint32_t part=0; part<sm->partitions.size(); ++part) {
            const auto& partition=sm->partitions[part];
            const auto* collector=partition.registers->collector();
            for (uint32_t index=0; index<32; ++index) {
                if (!partition.collector_owners[index] || !collector->selected(index)) continue;
                const auto& entry=*inflight_.at(*partition.collector_owners[index]);
                requests|=uint64_t{1}<<(static_cast<uint32_t>(entry.footprint.pipe)*8+part);
                if (entry.footprint.pipe==Pipe::async && !entry.mask) noop|=1u<<part;
            }
        }
        sm->execution_arbiter->arbitrate(requests,noop,bool(sm->barrier_job || sm->transaction_retiring));
    }
}

bool TimingMachine::execution_available(const Inflight& entry) {
    auto& sm=*sms_[entry.sm]; auto& partition=sm.partitions[entry.partition];
    const auto pipe=entry.footprint.pipe;
    if (sm.execution_arbiter)
        return entry.collector && partition.registers->collector()->selected(*entry.collector) &&
            sm.execution_arbiter->granted(static_cast<uint32_t>(pipe),entry.partition);
    if (pipe==Pipe::tensor && sm.tensor_owner) { ++stats_.unit_stalls; return false; }
    if (pipe==Pipe::async && !async_available(entry)) return false;
    if (partition.available[static_cast<uint32_t>(pipe)]>cycle_) { ++stats_.unit_stalls; return false; }
    return true;
}

// Confirm only accepted work; refused downstream ports neither consume intervals nor rotate shared priority.
// 仅确认实际接收的工作；下游拒绝不会消耗间隔或轮转共享优先级。
void TimingMachine::accept_execution(const Inflight& entry) {
    auto& sm=*sms_[entry.sm]; const auto pipe=entry.footprint.pipe;
    if (sm.execution_arbiter) sm.execution_arbiter->accept(static_cast<uint32_t>(pipe),entry.partition);
    else {
        const auto& c=machine_.config_.pipeline;
        const auto interval=pipe==Pipe::integer ? c.integer_interval : pipe==Pipe::floating ? c.float_interval :
            pipe==Pipe::special ? c.special_interval : 1;
        sm.partitions[entry.partition].available[static_cast<uint32_t>(pipe)]=cycle_+interval;
    }
    trace(entry,"execute");
}
}

#include "simt/timing.hpp"
#include "simt/execution.hpp"
#include <stdexcept>

namespace simt {
// Result readiness does not bypass register-file write bandwidth or predicate write ports.
// 结果就绪不能绕过寄存器堆写带宽和谓词写端口。
bool TimingMachine::writeback_available(Inflight& entry) {
    if (!cta_barrier_available(entry)) return false;
    if (entry.completion) return take_writeback(entry);
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    const auto& f = entry.footprint;
    if (!f.write_mask) return true;
    if (f.destination) {
        const bool available = partition.registers ? partition.registers->writable(*f.destination) :
            partition.ports.write(*f.destination, cycle_);
        if (!available) { ++stats_.writeback_stalls; return false; }
    }
    if (!f.predicate) return true;
    if (partition.registers) {
        if (partition.registers->predicate_writable()) return true;
        ++stats_.writeback_stalls; return false;
    }
    if (partition.predicate_write == cycle_) { ++stats_.writeback_stalls; return false; }
    partition.predicate_write = cycle_;
    return true;
}

void TimingMachine::commit(Inflight& entry) {
    const auto& f = entry.footprint;
    auto& warp = machine_.ctas_[entry.cta].warps[entry.warp];
    auto& timed = warps_[entry.cta][entry.warp];
    if (f.pipe == Pipe::control) commit_control(entry);
    else write_result(entry, warp);
    commit_register_data(entry);
    if (f.destination) timed.scoreboard->retire_write(*f.destination, f.write_mask);
    if (f.predicate) timed.scoreboard->retire_predicate(*f.predicate, f.write_mask);
    if (f.pipe == Pipe::async) commit_async_instruction(entry);
    if (f.pipe == Pipe::tensor) {
        advance_warp_pc(entry);
        if (sms_[entry.sm]->execution_arbiter) sms_[entry.sm]->execution_arbiter->release_tensor(entry.partition);
        else sms_[entry.sm]->tensor_owner.reset();
    }
    auto& scheduler=sms_[entry.sm]->partitions[entry.partition].scheduler;
    if (scheduler) scheduler->retire(timed.slot/sms_[entry.sm]->partitions.size(),f.boundary,f.pipe==Pipe::memory);
    else {
        --timed.inflight;
        if (f.boundary) timed.boundary = false;
        if (f.pipe == Pipe::memory) timed.memory = false;
    }
    ++stats_.retired; trace(entry, "retire");
}

void TimingMachine::retire_results() {
    prepare_writebacks();
    auto at = inflight_.begin();
    while (at != inflight_.end()) {
        auto& entry = *at->second;
        if (!entry.completion) poll_result(entry);
        if (entry.stage != Stage::writeback || !writeback_available(entry)) { ++at; continue; }
        try { commit(entry); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) +
                                     timing_location(entry.cta, entry.sm, entry.warp, entry.group.pc));
        }
        at = inflight_.erase(at);
    }
}
}

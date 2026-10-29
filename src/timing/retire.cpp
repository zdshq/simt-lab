#include "simt/timing.hpp"
#include "simt/execution.hpp"
#include <stdexcept>

namespace simt {
// Result readiness does not bypass register-file write bandwidth or predicate write ports.
// 结果就绪不能绕过寄存器堆写带宽和谓词写端口。
bool TimingMachine::writeback_available(Inflight& entry) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    const auto& f = entry.footprint;
    if (!f.write_mask) return true;
    if (f.destination && !partition.ports.write(*f.destination, cycle_)) {
        ++stats_.writeback_stalls; return false;
    }
    if (!f.predicate) return true;
    if (partition.predicate_write == cycle_) { ++stats_.writeback_stalls; return false; }
    partition.predicate_write = cycle_;
    return true;
}

void TimingMachine::commit(Inflight& entry) {
    const auto& f = entry.footprint;
    auto& warp = machine_.ctas_[entry.cta].warps[entry.warp];
    auto& timed = warps_[entry.cta][entry.warp];
    if (f.boundary) commit_control(entry);
    else for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(f.write_mask & lane_bit(lane))) continue;
        if (f.destination) write_register(warp, *f.destination, lane, entry.result.r[*f.destination][lane]);
        if (f.predicate) write_predicate(warp, *f.predicate, lane, entry.result.predicates[*f.predicate] & lane_bit(lane));
    }
    if (f.destination) timed.scoreboard.retire_write(*f.destination, f.write_mask);
    if (f.predicate) timed.scoreboard.retire_predicate(*f.predicate, f.write_mask);
    --timed.inflight;
    if (f.boundary) timed.boundary = false;
    if (f.pipe == Pipe::memory) timed.memory = false;
    ++stats_.retired; trace(entry, "retire");
}

void TimingMachine::retire_results() {
    auto at = inflight_.begin();
    while (at != inflight_.end()) {
        auto& entry = *at->second;
        if (entry.stage == Stage::memory) poll_memory(entry);
        if (entry.stage == Stage::executing && entry.due <= cycle_) entry.stage = Stage::writeback;
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

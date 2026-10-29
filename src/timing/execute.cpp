#include "simt/timing.hpp"
#include "simt/control.hpp"
#include "simt/memsync.hpp"
#include <stdexcept>

namespace simt {
// Each physical bank read transfers one vector operand into its collector before releasing WAR locks.
// 每次物理 bank 读取向收集器传送一个向量操作数，然后才释放 WAR 锁。
void TimingMachine::collect_operands() {
    for (auto& [id, pointer] : inflight_) {
        static_cast<void>(id); auto& entry = *pointer;
        if (entry.stage != Stage::collect) continue;
        auto& ports = sms_[entry.sm]->partitions[entry.partition].ports;
        auto use = entry.unread.begin();
        while (use != entry.unread.end()) {
            if (!ports.read(use->reg, cycle_)) { ++stats_.register_stalls; ++use; continue; }
            const auto& source = machine_.ctas_[entry.cta].warps[entry.warp].r[use->reg];
            for (uint32_t lane = 0; lane < lanes; ++lane)
                if (use->mask & lane_bit(lane)) entry.operands.r[use->reg][lane] = source[lane];
            warps_[entry.cta][entry.warp].scoreboard.collect_read(use->reg, use->mask);
            use = entry.unread.erase(use);
            trace(entry, "operand_read");
        }
        if (entry.unread.empty()) entry.stage = Stage::ready;
    }
}

void TimingMachine::evaluate_scalar(Inflight& entry) {
    auto& cta = machine_.ctas_[entry.cta];
    const ExecutionContext context{entry.instruction, cta, entry.operands, entry.result, machine_.config_,
                                   entry.warp, *machine_.global_, machine_.constant_};
    if (entry.instruction.op == Op::CVTA) { execute_memory(context, entry.mask); return; }
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (entry.mask & lane_bit(lane)) execute_scalar_lane(context, lane);
}

// The control pipe commits synchronization and PC transitions only after older work drains.
// 控制流水线仅在较早工作排空后提交同步和 PC 变化。
void TimingMachine::commit_control(Inflight& entry) {
    auto& cta = machine_.ctas_[entry.cta]; auto& warp = cta.warps[entry.warp];
    auto next = warp;
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (entry.group.mask & lane_bit(lane)) next.pc[lane] = entry.group.pc + 1;
    const ExecutionContext context{entry.instruction, cta, entry.operands, next, machine_.config_,
                                   entry.warp, *machine_.global_, machine_.constant_};
    if (!execute_control(context, entry.group, entry.mask, machine_.module_) &&
        !execute_collective(context, entry.group, entry.mask) && !execute_barrier(context, entry.mask) &&
        !execute_memory(context, entry.mask)) throw std::runtime_error("unhandled_timing_control");
    warp = std::move(next);
    release_cta_barriers(cta);
}

void TimingMachine::launch(Inflight& entry) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    const auto& config = machine_.config_.pipeline;
    const auto pipe = entry.footprint.pipe;
    const auto index = static_cast<size_t>(pipe);
    if (partition.available[index] > cycle_) { ++stats_.unit_stalls; return; }
    const uint32_t latency = pipe == Pipe::integer ? config.integer_latency :
        pipe == Pipe::floating ? config.float_latency : pipe == Pipe::special ? config.special_latency : 1;
    const uint32_t interval = pipe == Pipe::integer ? config.integer_interval :
        pipe == Pipe::floating ? config.float_interval : pipe == Pipe::special ? config.special_interval : 1;
    partition.available[index] = cycle_ + interval;
    if (pipe == Pipe::memory) { prepare_memory(entry); entry.stage = Stage::memory; }
    else {
        if (!entry.footprint.boundary) evaluate_scalar(entry);
        entry.stage = Stage::executing; entry.due = cycle_ + latency;
    }
    trace(entry, "execute");
}

void TimingMachine::launch_ready() {
    for (auto& [id, entry] : inflight_) {
        static_cast<void>(id);
        if (entry->stage != Stage::ready) continue;
        try { launch(*entry); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) +
                                     timing_location(entry->cta, entry->sm, entry->warp, entry->group.pc));
        }
    }
}
}

#include "simt/timing.hpp"
#include "simt/control.hpp"
#include "simt/memsync.hpp"
#include <stdexcept>

namespace simt {
// Each physical bank read transfers one vector operand into its collector before releasing WAR locks.
// 每次物理 bank 读取向收集器传送一个向量操作数，然后才释放 WAR 锁。
void TimingMachine::collect_operands() {
    collect_rtl_operands();
    for (auto& [id, pointer] : inflight_) {
        static_cast<void>(id); auto& entry = *pointer;
        if (entry.stage != Stage::collect || entry.collector) continue;
        auto use = entry.unread.begin();
        while (use != entry.unread.end()) {
            if (!collect_operand(entry, *use)) { ++stats_.register_stalls; ++use; continue; }
            warps_[entry.cta][entry.warp].scoreboard->collect_read(use->reg, use->mask);
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
    if (entry.collective_result) write_result(entry, next);
    else if (!commit_warp_control(entry, next) && !execute_control(context, entry.group, entry.mask, machine_.module_) &&
        !execute_collective(context, entry.group, entry.mask) && !execute_barrier(context, entry.mask) &&
        !execute_memory(context, entry.mask)) throw std::runtime_error("unhandled_timing_control");
    warp = std::move(next);
    release_cta_barriers(cta);
}

void TimingMachine::launch(Inflight& entry) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    const auto& config = machine_.config_.pipeline;
    const auto pipe = entry.footprint.pipe;
    if (pipe == Pipe::tensor && sms_[entry.sm]->tensor_owner) { ++stats_.unit_stalls; return; }
    if (pipe == Pipe::async && !async_available(entry)) return;
    const auto index = static_cast<size_t>(pipe);
    if (partition.available[index] > cycle_) { ++stats_.unit_stalls; return; }
    const uint32_t latency = pipe == Pipe::integer ? config.integer_latency :
        pipe == Pipe::floating ? config.float_latency : pipe == Pipe::special ? config.special_latency : 1;
    const uint32_t interval = pipe == Pipe::integer ? config.integer_interval :
        pipe == Pipe::floating ? config.float_interval : pipe == Pipe::special ? config.special_interval : 1;
    if (entry.collector && !partition.registers->collector()->selected(*entry.collector)) {
        ++stats_.unit_stalls; return;
    }
    if ((pipe == Pipe::integer || pipe == Pipe::floating) && entry.collector) {
        const bool accepted = pipe == Pipe::integer ? launch_integer(entry) : launch_floating(entry);
        if (!accepted) { ++stats_.unit_stalls; return; }
        partition.available[index] = cycle_ + interval;
        trace(entry, "execute"); return;
    }
    if (entry.collector && entry.instruction.op >= Op::ELECT && entry.instruction.op <= Op::ACTIVEMASK) {
        if (!launch_collective(entry)) { ++stats_.unit_stalls; return; }
        partition.available[index] = cycle_ + interval; trace(entry, "execute"); return;
    }
    take_collector(entry);
    partition.available[index] = cycle_ + interval;
    if (pipe == Pipe::async) start_async(entry);
    else if (pipe == Pipe::tensor) {
        sms_[entry.sm]->tensor_owner = entry.id; entry.tensor.emplace(); entry.stage = Stage::tensor;
    } else if (pipe == Pipe::memory) { prepare_memory(entry); entry.stage = Stage::memory; }
    else {
        if (!entry.footprint.boundary) evaluate_scalar(entry);
        else if (entry.completion && pipe == Pipe::control) {
            const ExecutionContext context{entry.instruction, machine_.ctas_[entry.cta], entry.operands, entry.result,
                machine_.config_, entry.warp, *machine_.global_, machine_.constant_};
            entry.collective_result = execute_collective(context, entry.group, entry.mask);
        }
        entry.stage = Stage::executing; entry.due = cycle_ + latency;
    }
    trace(entry, "execute");
}

void TimingMachine::launch_ready() {
    dispatch_collector_operands();
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

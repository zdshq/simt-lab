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
    const auto barrier_release = accept_cta_barrier(entry);
    const bool hardware_barrier = barrier_release && entry.instruction.op != Op::EXIT;
    const bool hardware_control = !entry.collective_result && commit_warp_control(entry, next);
    if (entry.collective_result) write_result(entry, next);
    else if (!hardware_control && !hardware_barrier && !execute_control(context, entry.group, entry.mask, machine_.module_) &&
        !execute_collective(context, entry.group, entry.mask) && !execute_barrier(context, entry.mask) &&
        !execute_memory(context, entry.mask)) throw std::runtime_error("unhandled_timing_control");
    warp = std::move(next);
    if (auto* unit = frontend(entry.cta, entry.warp); unit && !hardware_control)
        unit->advance({entry.group.pc, entry.group.mask});
    if (barrier_release) route_cta_barrier(entry, *barrier_release);
    else if (cta_barrier(entry.cta)) observe_frontend(entry.cta, entry.warp);
    else { release_cta_barriers(cta); publish_waiters(cta); }
}

void TimingMachine::launch(Inflight& entry) {
    const auto& config = machine_.config_.pipeline;
    const auto pipe = entry.footprint.pipe;
    if (!execution_available(entry)) return;
    const uint32_t latency = pipe == Pipe::integer ? config.integer_latency :
        pipe == Pipe::floating ? config.float_latency : pipe == Pipe::special ? config.special_latency : 1;
    if ((pipe == Pipe::integer || pipe == Pipe::floating) && entry.collector) {
        const bool accepted = pipe == Pipe::integer ? launch_integer(entry) : launch_floating(entry);
        if (accepted) accept_execution(entry);
        return;
    }
    if (entry.collector && entry.instruction.op >= Op::ELECT && entry.instruction.op <= Op::ACTIVEMASK) {
        if (launch_collective(entry)) accept_execution(entry);
        return;
    }
    take_collector(entry);
    if (pipe == Pipe::async) start_async(entry);
    else if (pipe == Pipe::tensor) {
        if (!sms_[entry.sm]->execution_arbiter) sms_[entry.sm]->tensor_owner = entry.id;
        entry.tensor.emplace(); entry.stage = Stage::tensor;
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
    accept_execution(entry);
}

void TimingMachine::launch_ready() {
    dispatch_collector_operands();
    arbitrate_execution();
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

#include "simt/timing.hpp"
#include "simt/async.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
bool overlaps(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes) {
    return a < b + b_bytes && b < a + a_bytes;
}

BarrierAction action(Op op) {
    switch (op) {
    case Op::MB_INIT: return BarrierAction::initialize;
    case Op::MB_EXPECT: return BarrierAction::expect;
    case Op::MB_ARRIVE: return BarrierAction::arrive;
    case Op::MB_WAIT: return BarrierAction::wait;
    case Op::CP_ASYNC: return BarrierAction::issue;
    default: throw std::runtime_error("invalid_async_opcode");
    }
}
}

// A full copy queue must not hold the control port needed to acknowledge older completions.
// 已满拷贝队列不能占住控制端口，否则较早的完成事务将无法获得确认。
bool TimingMachine::async_available(const Inflight& entry) {
    if (!entry.mask) return true;
    auto& sm = *sms_[entry.sm];
    if (sm.transaction_retiring) { ++sm.async_stats.barrier_stalls; return false; }
    if (sm.barrier_job) { ++sm.async_stats.barrier_stalls; return false; }
    const bool copy_full = sm.dma ? !sm.dma->available() : sm.copies.size() == machine_.config_.async.commands;
    if (entry.instruction.op == Op::CP_ASYNC && copy_full) {
        ++sm.async_stats.command_stalls; return false;
    }
    return true;
}

// Pending DMA ranges and accepted ordinary stores cannot become opaque records midway through writing.
// 未完成 DMA 区域和已接受普通写入不能在写入过程中变成不透明记录。
void TimingMachine::check_pending_shared_writes(uint32_t cta, uint32_t address, uint32_t bytes) const {
    for (const auto& sm : sms_) for (const auto& copy : sm->copies)
        if (copy.cta == cta && overlaps(address, bytes, copy.destination, copy.bytes))
            throw std::runtime_error("mbarrier_overlaps_pending_copy");
    for (const auto& [id, entry] : inflight_) {
        static_cast<void>(id);
        if (entry->cta != cta) continue;
        for (const auto& part : entry->memory) {
            if (part.done || part.space != 1 || !part.access.write) continue;
            for (uint32_t lane = 0; lane < lanes; ++lane)
                if ((part.access.mask & lane_bit(lane)) && overlaps(address, bytes, part.access.address[lane], part.access.width))
                    throw std::runtime_error("mbarrier_overlaps_pending_store");
        }
    }
}

// Reserve protection at admission, before the asynchronous record writes become visible.
// 在准入时预约保护，早于异步记录写入变为可见的时刻。
void TimingMachine::reserve_barrier_record(const Inflight& entry, uint32_t address) {
    auto& cta = machine_.ctas_[entry.cta]; auto& sm = *sms_[entry.sm];
    const auto expected = entry.instruction.imm;
    if (!expected || expected > 1024) throw std::runtime_error("invalid_mbarrier_count");
    cta.shared.check(address, 32, 16); check_shared_write(cta, address, 32);
    check_pending_shared_writes(entry.cta, address, 32);
    if (!sm.transactions && sm.barrier_entries == machine_.config_.async.barrier_slots) throw std::runtime_error("mbarrier_table_full");
    cta.reserved_transactions.insert(address); ++sm.barrier_entries;
}

// A single control job owns the instruction until its record update or wait query finishes.
// 一个控制任务持有指令，直到记录更新或等待查询完成。
void TimingMachine::start_async(Inflight& entry) {
    if (!entry.mask) { entry.stage = Stage::executing; entry.due = cycle_ + 1; return; }
    const auto& i = entry.instruction;
    if (i.op != Op::MB_WAIT && std::popcount(entry.mask) != 1) throw std::runtime_error("async_single_issuer_required");
    const auto lane = std::countr_zero(entry.mask);
    const auto address = read_register(entry.operands, i.op == Op::CP_ASYNC ? i.rc : i.ra, lane);
    if (i.op == Op::MB_INIT) reserve_barrier_record(entry, address);
    BarrierJob job{action(i.op)};
    job.instruction = entry.id; job.cta = entry.cta; job.address = address; job.remaining = entry.mask;
    job.due = cycle_ + machine_.config_.async.barrier_latency;
    sms_[entry.sm]->barrier_job = job; ++sms_[entry.sm]->async_stats.barrier_requests;
    entry.stage = Stage::async;
}

// Retirement advances continuation PCs; hardware wait state retains any completion received after its query.
// 退休推进后续 PC；硬件等待状态保留查询之后已经收到的完成事件。
void TimingMachine::commit_async_instruction(Inflight& entry) {
    auto& cta = machine_.ctas_[entry.cta]; auto& warp = cta.warps[entry.warp];
    advance_warp_pc(entry);
    if (sms_[entry.sm]->transactions) { observe_frontend(entry.cta, entry.warp); return; }
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (entry.instruction.op != Op::MB_WAIT || !(entry.mask & lane_bit(lane))) continue;
        warp.state[lane] = entry.result.state[lane]; warp.blocked_token[lane] = entry.result.blocked_token[lane];
        warp.waiting_phase[lane] = entry.result.waiting_phase[lane];
    }
    release_async_waiters(cta);
    publish_waiters(cta);
}
}

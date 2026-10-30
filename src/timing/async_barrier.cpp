#include "simt/timing.hpp"
#include "simt/async.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
// Validate a complete DMA range and reserve only bytes that were explicitly expected.
// 检查完整 DMA 范围，仅预约已明确声明预期的字节。
void TimingMachine::prepare_copy_issue(BarrierJob& job, const Inflight& entry) {
    const auto& i = entry.instruction; const auto lane = std::countr_zero(entry.mask);
    if (!i.imm || i.imm > 4096) throw std::runtime_error("invalid_async_copy_size");
    if (job.next.issued > job.next.bytes || i.imm > job.next.bytes - job.next.issued)
        throw std::runtime_error("async_exceeds_expected_bytes");
    const auto source = read_register(entry.operands, i.ra, lane);
    const auto destination = read_register(entry.operands, i.rb, lane);
    machine_.global_->check(source, i.imm); machine_.ctas_[entry.cta].shared.check(destination, i.imm);
    check_shared_write(machine_.ctas_[entry.cta], destination, i.imm);
    job.next.issued += i.imm;
}

// Work on a private next-state record; logical phase visibility waits for physical publication.
// 在私有下一状态记录上操作，逻辑阶段的可见性等待物理发布完成。
void TimingMachine::prepare_barrier_job(uint32_t sm, BarrierJob& job) {
    auto& cta = machine_.ctas_[job.cta];
    if (job.action == BarrierAction::complete) {
        const auto& copy = find_copy(sm, job.copy); job.next = transaction(cta, job.address);
        if (job.next.phase != copy.phase) throw std::runtime_error("async_phase_mismatch");
        if (job.next.issued < copy.bytes || job.next.bytes < copy.bytes) throw std::runtime_error("async_accounting_underflow");
        job.next.bytes -= copy.bytes; job.next.issued -= copy.bytes;
    } else {
        auto& entry = *inflight_.at(job.instruction); const auto count = entry.instruction.imm;
        if (job.action == BarrierAction::initialize) job.next = {0, count, count, 0, 0};
        else job.next = transaction(cta, job.address);
        if (job.action == BarrierAction::expect) {
            if (!count || count > UINT32_MAX - job.next.bytes) throw std::runtime_error("invalid_expected_bytes");
            job.next.bytes += count;
        } else if (job.action == BarrierAction::arrive) {
            if (!job.next.arrivals) throw std::runtime_error("mbarrier_arrival_overflow");
            write_register(entry.result, entry.instruction.rd, std::countr_zero(entry.mask), job.next.phase);
            --job.next.arrivals;
        } else if (job.action == BarrierAction::issue) prepare_copy_issue(job, entry);
    }
    job.phase_advanced = settle_transaction(job.next); job.prepared = true;
}

// The privileged record writer competes with payload and ordinary shared-memory traffic.
// 特权记录写入器与载荷和普通 shared 流量竞争端口。
void TimingMachine::publish_barrier_job(uint32_t sm_id, BarrierJob& job) {
    auto& sm = *sms_[sm_id]; auto& cta = machine_.ctas_[job.cta];
    if (!job.ticket) {
        const auto words = transaction_words(job.next);
        hw::WarpAccess access; access.mask = 0xFF; access.write = true;
        for (uint32_t lane = 0; lane < words.size(); ++lane) { access.address[lane] = job.address + lane * 4; access.value[lane] = words[lane]; }
        job.ticket = sm.shared.submit(access, &cta.shared);
    }
    if (!job.ticket || !sm.shared.take(*job.ticket)) return;
    finish_barrier_job(sm_id, job);
}

// Publish logical state only after the shared record write acknowledges all words.
// shared 记录全部 word 的写入确认后，才发布逻辑状态。
void TimingMachine::finish_barrier_job(uint32_t sm_id, BarrierJob& job) {
    auto& cta = machine_.ctas_[job.cta]; auto& sm = *sms_[sm_id];
    cta.transactions[job.address] = job.next; cta.reserved_transactions.erase(job.address);
    if (job.phase_advanced) ++sm.async_stats.phase_completions;
    if (job.action == BarrierAction::complete) finish_copy(sm_id, job.copy);
    else {
        auto& entry = *inflight_.at(job.instruction);
        if (job.action == BarrierAction::issue) submit_async_copy(sm_id, job);
        trace(entry, "barrier_publish"); entry.stage = Stage::writeback;
    }
    release_async_waiters(cta);
    publish_waiters(cta);
    sm.barrier_job.reset();
}

// One distinct barrier address is queried per control latency; same-address lanes share a lookup.
// 每段控制延迟查询一个不同屏障地址，同址 lane 共享一次查询。
void TimingMachine::query_async_wait(uint32_t sm_id, BarrierJob& job) {
    auto& entry = *inflight_.at(job.instruction);
    const auto first = std::countr_zero(job.remaining);
    const auto address = read_register(entry.operands, entry.instruction.ra, first);
    const auto& barrier = transaction(machine_.ctas_[entry.cta], address);
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(job.remaining & lane_bit(lane)) || read_register(entry.operands, entry.instruction.ra, lane) != address) continue;
        const auto phase = read_register(entry.operands, entry.instruction.rb, lane);
        if (phase > barrier.phase) throw std::runtime_error("mbarrier_future_phase");
        if (phase == barrier.phase) {
            entry.result.state[lane] = LaneState::async_barrier; entry.result.blocked_token[lane] = address;
            entry.result.waiting_phase[lane] = phase;
        }
        job.remaining &= ~lane_bit(lane);
    }
    ++sms_[sm_id]->async_stats.wait_queries;
    if (job.remaining) job.due = cycle_ + machine_.config_.async.barrier_latency;
    else { trace(entry, "barrier_wait_checked"); entry.stage = Stage::writeback; sms_[sm_id]->barrier_job.reset(); }
}

// Preserve the origin before completion can release its DMA command and control job.
// 在完成操作释放 DMA 命令及控制任务之前，保存原始指令位置。
void TimingMachine::progress_barrier(uint32_t sm_id) {
    auto& sm = *sms_[sm_id];
    if (!sm.barrier_job) return;
    if (sm.async_service ? !sm.async_service->read().ready : sm.barrier_job->due>cycle_) return;
    auto& job = *sm.barrier_job;
    const auto cta = job.cta;
    const auto* origin = job.instruction ? inflight_.at(job.instruction).get() : nullptr;
    const auto warp = origin ? origin->warp : find_copy(sm_id, job.copy).warp;
    const auto pc = origin ? origin->group.pc : find_copy(sm_id, job.copy).pc;
    try {
        if (sm.transactions) { progress_transaction_rtl(sm_id, job); return; }
        if (job.action == BarrierAction::wait) { query_async_wait(sm_id, job); return; }
        if (!job.prepared) prepare_barrier_job(sm_id, job);
        publish_barrier_job(sm_id, job);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) + timing_location(cta, sm_id, warp, pc));
    }
}
}

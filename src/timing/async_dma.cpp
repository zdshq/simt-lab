#include "simt/timing.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace simt {
// Queue validated addresses after the outstanding-byte reservation becomes visible.
// 在途字节预约变为可见后，将验证过的地址加入队列。
void TimingMachine::submit_async_copy(uint32_t sm_id, const BarrierJob& job) {
    const auto& entry = *inflight_.at(job.instruction); const auto& i = entry.instruction;
    const auto lane = std::countr_zero(entry.mask);
    DmaCommand copy{};
    copy.id = next_copy_++; copy.instruction = entry.id; copy.cta = entry.cta; copy.sm = sm_id;
    copy.warp = entry.warp; copy.pc = entry.group.pc; copy.source = read_register(entry.operands, i.ra, lane);
    copy.destination = read_register(entry.operands, i.rb, lane); copy.address = job.address;
    copy.phase = job.next.phase; copy.bytes = i.imm;
    auto& sm = *sms_[sm_id];
    if (sm.dma) sm.dma->submit({copy.id,copy.cta,copy.source,copy.destination,copy.address,copy.phase,copy.bytes});
    if (sm.resource_unit) sm.resource_unit->start_dma(copy.cta);
    sm.copies.push_back(copy); ++sm.async_stats.copies;
    sm.async_stats.peak_commands = std::max(sm.async_stats.peak_commands, uint64_t{sm.copies.size()});
    trace_copy(copy, "copy_submitted");
}

void TimingMachine::trace_copy(const DmaCommand& copy, const std::string& action) {
    Inflight event; event.id = copy.instruction; event.cta = copy.cta; event.sm = copy.sm;
    event.warp = copy.warp; event.group.pc = copy.pc; trace(event, action);
}

// Each fragment holds only returned bytes; source Memory is never captured or reread directly.
// 每个片段只保存已返回字节，不直接捕获或重读源 Memory。
bool TimingMachine::service_copy_fragment(DmaCommand& copy, DmaFragment& fragment) {
    auto& sm = *sms_[copy.sm]; const bool read = fragment.stage == FragmentStage::read;
    if (!fragment.ticket) {
        fragment.ticket = read ? submit_memory(copy.sm, fragment.access) :
            sm.shared->submit(fragment.access, &machine_.ctas_[copy.cta].shared);
        if (!fragment.ticket) { ++sm.async_stats.memory_stalls; return false; }
        trace_copy(copy, read ? "copy_read_submit" : "copy_write_submit");
    }
    const auto result = read ? memory_->take(*fragment.ticket) : sm.shared->take(*fragment.ticket);
    if (!result) return false;
    fragment.ticket.reset(); trace_copy(copy, read ? "copy_read_complete" : "copy_write_complete");
    if (!read) { sm.async_stats.write_bytes += fragment.bytes; return true; }
    sm.async_stats.read_bytes += fragment.bytes;
    fragment.access.value = *result; fragment.access.write = true; fragment.stage = FragmentStage::write;
    for (uint32_t lane = 0; lane < lanes; ++lane)
        fragment.access.address[lane] = uint64_t{copy.destination} + fragment.offset + lane * fragment.access.width;
    return false;
}

// Returned write acknowledgements release staging capacity, regardless of warp progress.
// 写入确认归还暂存容量，不依赖 warp 是否继续执行。
void TimingMachine::service_copy_fragments(uint32_t sm_id) {
    auto& sm = *sms_[sm_id];
    if (sm.dma) { service_dma_rtl(sm_id); return; }
    for (auto& copy : sm.copies) {
        auto at = copy.fragments.begin();
        while (at != copy.fragments.end()) {
            if (!service_copy_fragment(copy, *at)) { ++at; continue; }
            at = copy.fragments.erase(at); --sm.copy_fragments;
        }
    }
}

// One fragment per SM per clock enters the bounded read/write staging pool in round-robin order.
// 每 SM 每拍最多一个片段按轮转顺序进入有限读写暂存池。
void TimingMachine::admit_copy_fragment(uint32_t sm_id) {
    auto& sm = *sms_[sm_id]; const auto& config = machine_.config_.async;
    if (sm.dma) return;
    if (sm.copy_fragments == config.fragments) { ++sm.async_stats.fragment_stalls; return; }
    for (uint32_t offset = 0; offset < sm.copies.size(); ++offset) {
        const auto at = (sm.copy_cursor + offset) % sm.copies.size(); auto& copy = sm.copies[at];
        if (copy.cursor == copy.bytes) continue;
        const auto source = uint64_t{copy.source} + copy.cursor, destination = uint64_t{copy.destination} + copy.cursor;
        const uint32_t width = (source % 4 || destination % 4 || copy.bytes - copy.cursor < 4 || config.chunk_bytes < 4) ? 1 : 4;
        const auto bytes = std::min({copy.bytes - copy.cursor, config.chunk_bytes, lanes * width}) / width * width;
        hw::WarpAccess access; access.mask = 0; access.width = width;
        for (uint32_t lane = 0; lane < bytes / width; ++lane) { access.mask |= lane_bit(lane); access.address[lane] = source + lane * width; }
        copy.fragments.push_back({copy.cursor, bytes, access, {}, FragmentStage::read}); copy.cursor += bytes;
        ++sm.copy_fragments; ++sm.async_stats.fragments;
        sm.copy_cursor = static_cast<uint32_t>((at + 1) % sm.copies.size()); return;
    }
}

DmaCommand& TimingMachine::find_copy(uint32_t sm_id, uint64_t id) {
    for (auto& copy : sms_[sm_id]->copies) if (copy.id == id) return copy;
    throw std::runtime_error("unknown_dma_command");
}

// Completion requests outrank new instructions so a saturated DMA queue can always release credits.
// 完成请求优先于新指令，使饱和 DMA 队列始终能够归还额度。
void TimingMachine::complete_ready_copy(uint32_t sm_id) {
    auto& sm = *sms_[sm_id];
    if (sm.async_service) { schedule_async_background(sm_id); return; }
    if (sm.barrier_job) return;
    for (const auto& copy : sm.copies) {
        if (copy.cursor != copy.bytes || !copy.fragments.empty()) continue;
        BarrierJob job{BarrierAction::complete}; job.copy = copy.id; job.cta = copy.cta; job.address = copy.address;
        job.due = cycle_ + machine_.config_.async.barrier_latency;
        sm.barrier_job = job; ++sm.async_stats.barrier_requests; return;
    }
}

void TimingMachine::finish_copy(uint32_t sm_id, uint64_t id) {
    auto& sm = *sms_[sm_id];
    for (auto at = sm.copies.begin(); at != sm.copies.end(); ++at) {
        if (at->id != id) continue;
        if (sm.resource_unit) sm.resource_unit->finish_dma(at->cta);
        trace_copy(*at, "copy_complete"); ++sm.async_stats.completed; sm.copies.erase(at); return;
    }
    throw std::runtime_error("unknown_dma_completion");
}

// Advance independently of runnable lanes; completions claim the port before new instructions.
// 独立于可运行 lane 推进，完成事务在新指令之前申请端口。
void TimingMachine::progress_async_units() {
    for (uint32_t sm = 0; sm < sms_.size(); ++sm) {
        progress_transaction_retirement(sm);
        progress_barrier(sm); service_copy_fragments(sm); admit_copy_fragment(sm); complete_ready_copy(sm);
    }
}

bool TimingMachine::pending_copy(uint32_t cta) const {
    for (const auto& sm : sms_) for (const auto& copy : sm->copies) if (copy.cta == cta) return true;
    return false;
}

bool TimingMachine::background_work() const {
    for (const auto& sm : sms_) {
        if (sm->resource_unit && (sm->resource_unit->pending() || sm->resource_unit->read().releasing)) return true;
        if (sm->instruction_fetch && sm->instruction_fetch->busy()) return true;
        const bool copies=sm->dma ? sm->dma->commands()!=0 : !sm->copies.empty();
        if (copies || sm->barrier_job || (sm->async_service && sm->async_service->read().pending)) return true;
    }
    return false;
}
}

#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
// Each external return bus accepts at most one response per cycle; tickets retain other completed responses.
// 每条外部返回总线每拍最多接收一个响应；票据保留其余已完成响应。
void TimingMachine::return_dma_rtl(uint32_t sm_id,bool write) {
    auto& sm=*sms_[sm_id]; auto& tickets=write ? sm.dma_writes : sm.dma_reads;
    auto& cursor=sm.dma_return_cursor[write ? 1 : 0];
    for (uint32_t offset=0; offset<64; ++offset) {
        const auto slot=(cursor+offset)%64; if (!tickets[slot]) continue;
        const auto result=write ? sm.shared.take(*tickets[slot]) : memory_->take(*tickets[slot]);
        if (!result) continue;
        const auto event=write ? sm.dma->returned_write(slot) : sm.dma->returned_read(slot,*result);
        tickets[slot].reset(); cursor=(slot+1)%64;
        trace_copy(find_copy(sm_id,event.id),write ? "copy_write_complete" : "copy_read_complete"); return;
    }
}

// Addresses, masks and write payload come directly from held hardware outputs.
// 地址、掩码与写载荷直接来自硬件保持的输出。
void TimingMachine::request_dma_rtl(uint32_t sm_id,bool write) {
    auto& sm=*sms_[sm_id]; const auto request=sm.dma->request(write); if (!request) return;
    auto& tickets=write ? sm.dma_writes : sm.dma_reads;
    if (tickets[request->slot]) throw std::runtime_error("dma_ticket_slot_busy");
    const auto ticket=write ? sm.shared.submit(request->access,&machine_.ctas_[request->cta].shared) :
        submit_memory(sm_id,request->access);
    if (!ticket) { ++sm.async_stats.memory_stalls; return; }
    tickets[request->slot]=*ticket; sm.dma->accept(write);
    trace_copy(find_copy(sm_id,request->id),write ? "copy_write_submit" : "copy_read_submit");
}
void TimingMachine::service_dma_rtl(uint32_t sm) {
    return_dma_rtl(sm,false); return_dma_rtl(sm,true);
    request_dma_rtl(sm,false); request_dma_rtl(sm,true);
}

// Claiming a completed command does not free its slot; the transaction publication later returns that credit.
// 领取完成命令不会释放其槽位；后续事务发布才归还该额度。
void TimingMachine::complete_dma_rtl(uint32_t sm_id) {
    auto& sm=*sms_[sm_id]; const auto completion=sm.dma->completion(); if (!completion) return;
    BarrierJob job{BarrierAction::complete}; job.copy=completion->id; job.cta=completion->cta;
    job.address=completion->barrier; job.dma_completion=*completion;
    job.due=cycle_+machine_.config_.async.barrier_latency;
    sm.dma->claim(); sm.barrier_job=job; ++sm.async_stats.barrier_requests;
}
}

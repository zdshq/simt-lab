#include "simt/timing.hpp"
#include "simt/async.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
hw::TransactionOp operation(BarrierAction action) {
    switch (action) {
    case BarrierAction::initialize: return hw::TransactionOp::initialize;
    case BarrierAction::expect: return hw::TransactionOp::expect;
    case BarrierAction::arrive: return hw::TransactionOp::arrive;
    case BarrierAction::issue: return hw::TransactionOp::issue;
    case BarrierAction::wait: return hw::TransactionOp::query;
    case BarrierAction::complete: return hw::TransactionOp::complete;
    }
    throw std::runtime_error("invalid_async_action");
}
}

// Only raw instruction operands and actual DMA completion descriptors enter the hardware table.
// 仅原始指令操作数与实际 DMA 完成描述符进入硬件表。
void TimingMachine::submit_transaction_rtl(uint32_t sm, BarrierJob& job) {
    auto& unit=*sms_[sm]->transactions;
    if (!unit.available()) return;
    hw::TransactionCommand command{operation(job.action),job.cta,job.address};
    if (job.action==BarrierAction::complete) {
        if (job.dma_completion) { command.amount=job.dma_completion->bytes; command.phase=job.dma_completion->phase; }
        else { const auto& copy=find_copy(sm,job.copy); command.amount=copy.bytes; command.phase=copy.phase; }
    } else {
        const auto& entry=*inflight_.at(job.instruction); const auto& i=entry.instruction;
        command.amount=i.imm;
        if (job.action==BarrierAction::wait) {
            job.address=read_register(entry.operands,i.ra,std::countr_zero(job.remaining)); command.address=job.address;
        }
    }
    unit.submit(command); job.submitted=true;
}

// Publication uses the circuit's pending record and existing shared ports; visibility waits for acknowledgement.
// 发布使用电路的待提交记录与既有 shared 端口，可见性等待确认。
void TimingMachine::publish_transaction_rtl(uint32_t sm_id, BarrierJob& job) {
    auto& sm=*sms_[sm_id]; const auto publication=sm.transactions->publication();
    if (!publication || job.publication_done) return;
    if (!job.prepared) {
        const auto& r=publication->record;
        job.next={r.phase,r.expected,r.arrivals,r.bytes,r.issued}; job.phase_advanced=publication->advanced;
        if (job.action==BarrierAction::issue) {
            const auto& entry=*inflight_.at(job.instruction); const auto& i=entry.instruction;
            const auto lane=std::countr_zero(entry.mask);
            const auto destination=read_register(entry.operands,i.rb,lane);
            machine_.global_->check(read_register(entry.operands,i.ra,lane),i.imm);
            machine_.ctas_[entry.cta].shared.check(destination,i.imm);
            check_shared_write(machine_.ctas_[entry.cta],destination,i.imm);
        }
        job.prepared=true;
    }
    if (!job.ticket) {
        const auto words=transaction_words(job.next); hw::WarpAccess access; access.mask=0xff; access.write=true;
        for (uint32_t lane=0; lane<words.size(); ++lane) { access.address[lane]=job.address+lane*4; access.value[lane]=words[lane]; }
        job.ticket=sm.shared->submit(access,&machine_.ctas_[job.cta].shared);
    }
    if (!job.ticket || !sm.shared->take(*job.ticket)) return;
    sm.transactions->acknowledge(); job.publication_done=true;
}

// One queried address feeds raw lane tokens to the frontend's hardware phase comparators.
// 一个被查询地址将各 lane 原始令牌送到前端硬件阶段比较器。
void TimingMachine::query_transaction_rtl(uint32_t sm, BarrierJob& job, uint32_t phase) {
    auto& entry=*inflight_.at(job.instruction); uint32_t mask=0; std::array<uint32_t,32> phases{};
    for (uint32_t lane=0; lane<32; ++lane) {
        if (!(job.remaining&lane_bit(lane)) || read_register(entry.operands,entry.instruction.ra,lane)!=job.address) continue;
        mask|=lane_bit(lane); phases[lane]=read_register(entry.operands,entry.instruction.rb,lane);
    }
    frontend(entry.cta,entry.warp)->async_wait(mask,job.address,phase,phases);
    observe_frontend(entry.cta,entry.warp); job.remaining&=~mask; ++sms_[sm]->async_stats.wait_queries;
    if (job.remaining) { job.submitted=false; sms_[sm]->async_service->retry(); }
    else {
        trace(entry,"barrier_wait_checked"); entry.stage=Stage::writeback;
        sms_[sm]->async_service->complete(); sms_[sm]->barrier_job.reset();
    }
}

void TimingMachine::route_transaction_release(uint32_t cta_id, uint32_t address, uint32_t phase) {
    const auto& cta=machine_.ctas_[cta_id];
    for (uint32_t wi=0; wi<cta.warps.size(); ++wi) {
        frontend(cta_id,wi)->async_release(address,phase); observe_frontend(cta_id,wi);
    }
}

// The architectural map is an observation used for dumps/protection; counters are never read back into the RTL.
// 架构映射仅用于转储和记录保护；计数器绝不作为输入回填 RTL。
void TimingMachine::finish_transaction_rtl(uint32_t sm_id, BarrierJob& job, const hw::TransactionResult& result) {
    if (result.error) throw std::runtime_error(hw::transaction_error(result.error));
    if (job.action==BarrierAction::wait) { query_transaction_rtl(sm_id,job,result.record.phase); return; }
    auto& sm=*sms_[sm_id]; auto& cta=machine_.ctas_[job.cta]; const auto& r=result.record;
    if (job.action==BarrierAction::initialize) sm.resource_unit->initialize_table(job.cta);
    job.next={r.phase,r.expected,r.arrivals,r.bytes,r.issued};
    cta.transactions[job.address]=job.next; cta.reserved_transactions.erase(job.address);
    if (result.advanced) { ++sm.async_stats.phase_completions; route_transaction_release(job.cta,job.address,r.phase); }
    if (job.action==BarrierAction::complete) {
        if (job.dma_completion) sm.dma->release(*job.dma_completion);
        finish_copy(sm_id,job.copy);
    }
    else {
        auto& entry=*inflight_.at(job.instruction);
        if (job.action==BarrierAction::arrive) write_register(entry.result,entry.instruction.rd,std::countr_zero(entry.mask),result.token);
        if (job.action==BarrierAction::issue) submit_async_copy(sm_id,job);
        trace(entry,"barrier_publish"); entry.stage=Stage::writeback;
    }
    sm.async_service->complete(); sm.barrier_job.reset();
}

void TimingMachine::progress_transaction_rtl(uint32_t sm, BarrierJob& job) {
    if (!job.submitted) { submit_transaction_rtl(sm,job); return; }
    publish_transaction_rtl(sm,job);
    if (const auto result=sms_[sm]->transactions->take()) finish_transaction_rtl(sm,job,*result);
}

// Validate all owned records before a second scan returns slots; a rejected retirement cannot partially free them.
// 先验证全部所属记录，再用第二遍扫描归还槽位；被拒绝的退休不能部分释放记录。
bool TimingMachine::retire_transaction_cta(const Cta& cta) {
    auto& sm=*sms_[cta.sm_id];
    if (!sm.transactions || !sm.resource_unit->query(cta.block_id)->table_used) return true;
    if (!sm.resource_unit->query(cta.block_id)->table_clean) return false;
    const auto owner=sm.async_service->read();
    if (owner.kind!=hw::AsyncOwner::retirement || owner.cta!=cta.block_id || !owner.retirement_done) return false;
    sm.async_service->acknowledge_retirement(); return true;
}
void TimingMachine::progress_transaction_retirement(uint32_t sm_id) {
    auto& sm=*sms_[sm_id];
    if (!sm.async_service) return;
    const auto owner=sm.async_service->read();
    if (owner.kind!=hw::AsyncOwner::retirement || owner.retirement_done) return;
    const auto result=sm.transactions->take(); if (!result) return;
    if (result->error) throw std::runtime_error(hw::transaction_error(result->error));
    sm.resource_unit->clean_table(owner.cta);
    sm.async_service->complete();
}
}

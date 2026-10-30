#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
// Host wiring describes eligible physical CTA slots; the circuit chooses their service order.
// 宿主连线描述符合条件的物理 CTA 槽，电路决定服务顺序。
void TimingMachine::offer_async_service(uint32_t sm_id) {
    auto& sm=*sms_[sm_id]; hw::AsyncServiceInputs inputs;
    inputs.table_ready=sm.transactions->available();
    const auto owner=sm.async_service->read();
    for (uint32_t slot=0; slot<sm.cta_owners.size(); ++slot) {
        if (!sm.cta_owners[slot]) continue;
        const auto cta=*sm.cta_owners[slot]; inputs.retire_ctas[slot]=cta;
        if (scheduler_releasing_[cta] || machine_.ctas_[cta].transactions.empty()) continue;
        if (owner.kind==hw::AsyncOwner::retirement && owner.cta==cta) continue;
        if (cta_drained(machine_.ctas_[cta])) inputs.retire_requests|=uint64_t{1}<<slot;
    }
    if (const auto completion=sm.dma->completion()) {
        inputs.dma_valid=true; inputs.dma_id=completion->id; inputs.dma_cta=completion->cta;
    }
    sm.async_service->offer(inputs);
}

// Only the hardware-selected source may claim the table; completion and release share one owner.
// 只有硬件选中的来源可以取得事务表；完成与回收共享一个所有者。
void TimingMachine::schedule_async_background(uint32_t sm_id) {
    auto& sm=*sms_[sm_id]; offer_async_service(sm_id);
    const auto selected=sm.async_service->selection();
    if (selected.kind==hw::AsyncOwner::dma) { complete_dma_rtl(sm_id); return; }
    if (selected.kind!=hw::AsyncOwner::retirement) return;
    sm.async_service->accept_background();
    sm.transactions->submit({hw::TransactionOp::release,selected.cta});
}
}

#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
// Resource-controller outputs drive retirement requests without software lane or DMA reductions.
// 资源控制器输出直接驱动退休请求，不使用软件 lane 或 DMA 归约。
void TimingMachine::offer_async_service(uint32_t sm_id) {
    auto& sm=*sms_[sm_id]; hw::AsyncServiceInputs inputs;
    inputs.table_ready=sm.transactions->available();
    refresh_resources(sm_id);
    const auto resources=sm.resource_unit->read();
    inputs.retire_ctas=resources.owners; inputs.retire_requests=resources.cleanup;
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

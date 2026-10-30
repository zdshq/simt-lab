#include "simt/timing.hpp"
#include <bit>

namespace simt {
// Preserve physical lane and scheduler signals; the resource circuit computes CTA eligibility.
// 保留物理 lane 和调度器信号，由资源电路计算 CTA 资格。
void TimingMachine::refresh_resources(uint32_t sm_id) {
    auto& sm=*sms_[sm_id];
    if (!sm.resource_unit) return;
    hw::CtaResourceInputs inputs;
    for (uint32_t part=0; part<sm.partitions.size(); ++part) {
        const auto scheduler=sm.partitions[part].scheduler->read();
        for (uint32_t slot=part; slot<32; slot+=sm.partitions.size())
            inputs.inflight[slot]=scheduler.inflight[slot/sm.partitions.size()];
    }
    for (uint32_t slot=0; slot<32 && slot<sm.controls.size(); ++slot)
        if (sm.controls[slot]) inputs.states[slot]=sm.controls[slot]->read().control.lanes.state;
    sm.resource_unit->observe(inputs);
}

// Kernel completion includes the final edge that actually returns the hardware credits.
// kernel 完成包含真正归还硬件额度的最后一个边沿。
bool TimingMachine::resources_empty() const {
    for (const auto& sm : sms_)
        if (sm->resource_unit && (sm->resource_unit->pending() || sm->resource_unit->read().live)) return false;
    return true;
}

void TimingMachine::dump_cta_resources(std::ostream& out) const {
    out<<",\"cta_resources\":[";
    for (uint32_t sm=0; sm<sms_.size(); ++sm) {
        if (sm) out<<',';
        const auto& unit=sms_[sm]->resource_unit;
        if (!unit) { out<<"{\"backend\":\"cpp\"}"; continue; }
        const auto state=unit->read(); const auto& s=unit->stats();
        out<<"{\"backend\":\"rtl\",\"physical_cta_slots\":64,\"physical_warps\":32"
           <<",\"clocks\":"<<s.clocks<<",\"active_clocks\":"<<s.active_clocks
           <<",\"admitted\":"<<s.admitted<<",\"released\":"<<s.released<<",\"retiring\":"<<s.retiring
           <<",\"admission_stalls\":"<<s.admission_stalls<<",\"table_initializations\":"<<s.table_initializations
           <<",\"table_cleanups\":"<<s.table_cleanups<<",\"dma_started\":"<<s.dma_started<<",\"dma_finished\":"<<s.dma_finished
           <<",\"peak_ctas\":"<<s.peak_ctas<<",\"peak_registers\":"<<s.peak_registers<<",\"peak_shared\":"<<s.peak_shared
           <<",\"live\":"<<state.live<<",\"cleanup\":"<<state.cleanup<<",\"releasing\":"<<state.releasing
           <<",\"ctas\":"<<state.ctas<<",\"warps\":"<<std::popcount(state.warps)
           <<",\"registers\":"<<state.registers<<",\"shared\":"<<state.shared<<",\"pending\":"<<unit->pending()<<'}';
    }
    out<<']';
}
}

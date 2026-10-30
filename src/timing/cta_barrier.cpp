#include "simt/timing.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt {
namespace {
bool uses_barrier_table(Op op) { return op==Op::BAR_SYNC || op==Op::BAR_ARRIVE || op==Op::EXIT; }
}

// Physical CTA table slots survive block retirement and are cleared before their next owner starts.
// 物理 CTA 表槽在 block 退休后保留，在下个所有者启动前清零。
void TimingMachine::admit_cta_barrier(uint32_t cta, uint32_t sm_id) {
    auto& sm=*sms_[sm_id];
    if (sm.cta_tables.empty()) return;
    const auto found=std::find(sm.cta_owners.begin(),sm.cta_owners.end(),std::nullopt);
    if (found==sm.cta_owners.end()) throw std::runtime_error("cta_barrier_slot_exhausted");
    const auto slot=static_cast<uint32_t>(found-sm.cta_owners.begin());
    auto& table=sm.cta_tables[slot];
    if (!table) table=hw::make_cta_barrier();
    table->initialize(machine_.config_.threads); *found=cta; cta_slots_[cta]=slot;
}
hw::CtaBarrierEngine* TimingMachine::cta_barrier(uint32_t cta) const {
    auto& sm=*sms_[machine_.ctas_[cta].sm_id];
    return sm.cta_tables.empty() ? nullptr : sm.cta_tables[cta_slots_[cta]].get();
}

// Check the single table port before consuming an irreversible writeback acknowledgement.
// 在消费不可撤回的写回确认之前，检查单个表端口。
bool TimingMachine::cta_barrier_available(const Inflight& entry) {
    auto* table=cta_barrier(entry.cta);
    if (!table || !uses_barrier_table(entry.instruction.op) || table->available()) return true;
    ++stats_.cta_barrier_stalls; return false;
}
std::optional<uint32_t> TimingMachine::accept_cta_barrier(const Inflight& entry) {
    auto* table=cta_barrier(entry.cta);
    if (!table || !uses_barrier_table(entry.instruction.op)) return {};
    const auto& i=entry.instruction;
    return table->execute({i.op==Op::EXIT,entry.warp,entry.mask,i.rd,i.imm});
}

// Route the unmodified release bits to each physical warp; local hardware compares its stored wait identifiers.
// 将未改动的释放位送到各物理 warp，由本地硬件比较其已存等待编号。
void TimingMachine::route_cta_barrier(const Inflight& entry, uint32_t release) {
    auto& cta=machine_.ctas_[entry.cta];
    for (uint32_t wi=0; wi<cta.warps.size(); ++wi) {
        const uint32_t blocking=wi==entry.warp && entry.instruction.op==Op::BAR_SYNC ? entry.mask : 0;
        if (blocking || release) frontend(entry.cta,wi)->cta_barrier(blocking,entry.instruction.rd,release);
        observe_frontend(entry.cta,wi);
    }
}
}

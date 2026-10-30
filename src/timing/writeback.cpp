#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
// Reserve every instruction before execution so physical slot reuse never changes its arbitration age.
// 执行前为每条指令预约，避免物理槽复用改变仲裁年龄。
void TimingMachine::reserve_writeback(Inflight& entry) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    auto* unit = partition.registers ? partition.registers->writeback() : nullptr;
    if (!unit) return;
    const auto& f = entry.footprint;
    if (f.destination && f.predicate) throw std::runtime_error("writeback_multiple_destinations_unsupported");
    const auto warp = warps_[entry.cta][entry.warp].slot / sms_[entry.sm]->partitions.size();
    const uint32_t kind = f.destination ? 1 : f.predicate ? 2 : 0;
    entry.completion = unit->reserve({static_cast<uint32_t>(warp), static_cast<uint32_t>(f.pipe), kind,
                                    f.destination.value_or(f.predicate.value_or(0)), f.write_mask});
}

// Software producers submit here; integer, floating and collective RTL own their directly connected completion data.
// 软件生产端在这里提交；整数、浮点与集合 RTL 持有自己的直连完成数据。
void TimingMachine::submit_writeback(Inflight& entry) {
    if (!entry.completion || entry.result_submitted || entry.stage != Stage::writeback) return;
    const auto& f = entry.footprint;
    hw::RegisterVector values{};
    if (f.destination) values = entry.result.r[*f.destination];
    if (f.predicate) values[0] = entry.result.predicates[*f.predicate];
    auto* unit = sms_[entry.sm]->partitions[entry.partition].registers->writeback();
    entry.result_submitted = unit->complete(*entry.completion, static_cast<uint32_t>(f.pipe), values);
    if (entry.result_submitted) {
        if (f.destination) entry.result.r[*f.destination].fill(0);
        if (f.predicate) entry.result.predicates[*f.predicate] = 0;
    }
}

void TimingMachine::prepare_writebacks() {
    for (auto& [id, pointer] : inflight_) {
        static_cast<void>(id); auto& entry = *pointer;
        if (!entry.completion) continue;
        try {
            poll_result(entry);
            submit_writeback(entry);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) + timing_location(entry.cta, entry.sm, entry.warp, entry.group.pc));
        }
    }
    for (auto& sm : sms_) for (auto& partition : sm->partitions)
        if (partition.registers && partition.registers->writeback()) partition.registers->writeback()->arbitrate();
}

// Software-only backends retain their existing poll/retire order; RTL polls all producers before arbitration.
// 纯软件后端保留原有查询与退休顺序；RTL 在仲裁前查询全部生产端口。
void TimingMachine::poll_result(Inflight& entry) {
    if (entry.stage == Stage::integer || entry.stage == Stage::floating || entry.stage == Stage::collective) poll_rtl_execution(entry);
    if (entry.stage == Stage::memory) poll_memory(entry);
    if (entry.stage == Stage::tensor) poll_tensor(entry);
    if (entry.stage == Stage::executing && entry.due <= cycle_) entry.stage = Stage::writeback;
}

bool TimingMachine::take_writeback(Inflight& entry) {
    auto* unit = sms_[entry.sm]->partitions[entry.partition].registers->writeback();
    if (!entry.result_submitted || !unit->selected(*entry.completion)) { ++stats_.writeback_stalls; return false; }
    const auto values = unit->take(*entry.completion);
    if (entry.footprint.destination) entry.result.r[*entry.footprint.destination] = values;
    if (entry.footprint.predicate) entry.result.predicates[*entry.footprint.predicate] = values[0];
    return true;
}
}

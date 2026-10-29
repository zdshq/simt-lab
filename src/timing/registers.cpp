#include "simt/timing.hpp"
#include "simt/execution.hpp"

namespace simt {
// RTL reads are authoritative; the architectural register mirror never supplies this backend's operands.
// RTL 读取是权威数据源；架构寄存器镜像不向此后端提供操作数。
bool TimingMachine::collect_operand(Inflight& entry, const hw::RegisterUse& use) {
    auto& partition = sms_[entry.sm]->partitions[entry.partition];
    hw::RegisterVector values;
    if (partition.registers) {
        const auto slot = warps_[entry.cta][entry.warp].slot / sms_[entry.sm]->partitions.size();
        const auto data = partition.registers->read(slot, use.reg);
        if (!data) return false;
        values = *data;
    } else {
        if (!partition.ports.read(use.reg, cycle_)) return false;
        values = machine_.ctas_[entry.cta].warps[entry.warp].r[use.reg];
    }
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (use.mask & lane_bit(lane)) entry.operands.r[use.reg][lane] = values[lane];
    return true;
}

// Without RTL completion storage, control collectives still provide their destination during commit.
// 没有 RTL 完成存储时，控制类集合指令仍在提交时提供目标值。
void TimingMachine::commit_register_data(Inflight& entry) {
    auto& file = sms_[entry.sm]->partitions[entry.partition].registers;
    const auto& f = entry.footprint;
    if (!file || entry.completion || !f.destination || !f.write_mask) return;
    const auto slot = warps_[entry.cta][entry.warp].slot / sms_[entry.sm]->partitions.size();
    const auto& result = f.pipe == Pipe::control ? machine_.ctas_[entry.cta].warps[entry.warp] : entry.result;
    file->write(slot, *f.destination, f.write_mask, result.r[*f.destination]);
}

// Architectural dumps mirror accepted results; RTL operand reads still come from physical RF storage.
// 架构转储镜像已确认的结果；RTL 操作数读取仍来自物理寄存器堆存储。
void TimingMachine::write_result(const Inflight& entry, Warp& warp) const {
    const auto& f = entry.footprint;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(f.write_mask & lane_bit(lane))) continue;
        if (f.destination) write_register(warp, *f.destination, lane, entry.result.r[*f.destination][lane]);
        if (f.predicate) write_predicate(warp, *f.predicate, lane, entry.result.predicates[*f.predicate] & lane_bit(lane));
    }
}
}

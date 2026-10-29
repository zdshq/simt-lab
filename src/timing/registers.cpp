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

// Control collectives compute their destination during commit; all other pipes use their result latch.
// 控制类集合指令在提交时计算目标值，其他流水线使用各自的结果锁存值。
void TimingMachine::commit_register_data(Inflight& entry) {
    auto& file = sms_[entry.sm]->partitions[entry.partition].registers;
    const auto& f = entry.footprint;
    if (!file || !f.destination || !f.write_mask) return;
    const auto slot = warps_[entry.cta][entry.warp].slot / sms_[entry.sm]->partitions.size();
    const auto& result = f.pipe == Pipe::control ? machine_.ctas_[entry.cta].warps[entry.warp] : entry.result;
    file->write(slot, *f.destination, f.write_mask, result.r[*f.destination]);
}
}

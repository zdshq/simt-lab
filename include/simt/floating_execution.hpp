#pragma once
#include <cstdint>

namespace simt::hw {
// Only metadata crosses this interface; collected operands and rounded results remain inside RTL.
// 只有元数据经过此接口；已收集操作数和舍入后的结果始终留在 RTL 内部。
struct FloatingCommand {
    uint32_t collector = 0, completion = 0, opcode = 0, mode = 0;
    uint32_t ra = 0, rb = 0, rc = 0, mask = 0, latency = 6;
};
struct FloatingStats {
    uint64_t clocks = 0, active_clocks = 0, accepted = 0, completed = 0;
};
class FloatingExecution {
public:
    virtual ~FloatingExecution() = default;
    virtual bool launch(const FloatingCommand& command) = 0;
    virtual uint32_t occupancy() const = 0;
    virtual const FloatingStats& floating_stats() const = 0;
};
}

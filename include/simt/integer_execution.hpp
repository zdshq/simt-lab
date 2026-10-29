#pragma once
#include <cstdint>

namespace simt::hw {
// Metadata selects an already collected vector; this interface cannot inject arithmetic operands or results.
// 元数据选择已经收集的向量；此接口不能注入算术操作数或结果。
struct IntegerCommand {
    uint32_t collector = 0, completion = 0, opcode = 0, mode = 0;
    uint32_t ra = 0, rb = 0, rc = 0, mask = 0, immediate = 0, latency = 4;
    uint32_t warp = 0, block = 0, sm = 0, threads = 0, blocks = 0;
};
struct IntegerStats {
    uint64_t clocks = 0, active_clocks = 0, accepted = 0, completed = 0;
};
class IntegerExecution {
public:
    virtual ~IntegerExecution() = default;
    virtual bool launch(const IntegerCommand& command) = 0;
    virtual uint32_t occupancy() const = 0;
    virtual const IntegerStats& integer_stats() const = 0;
};
}

#pragma once
#include "simt/register_file.hpp"

namespace simt::hw {
// Kinds are 0=no write, 1=GPR and 2=predicate; predicate lane bits occupy payload word zero.
// 类型为 0=无写入、1=普通寄存器、2=谓词；谓词 lane 位放在载荷第零个字中。
struct WritebackDestination {
    uint32_t warp = 0, pipe = 0, kind = 0, reg = 0, mask = 0;
};
struct WritebackStats {
    uint64_t clocks = 0, active_clocks = 0, reserved = 0, completed = 0, retired = 0;
};

// Reserve at issue, submit producer results, seal the cycle, then acknowledge only circuit-selected entries.
// 发射时预约，提交生产端口结果，封闭本拍输入，然后只确认电路选中的条目。
class Writeback {
public:
    virtual ~Writeback() = default;
    virtual bool available() const = 0;
    virtual uint32_t reserve(const WritebackDestination& destination) = 0;
    virtual bool complete(uint32_t entry, uint32_t pipe, const RegisterVector& values) = 0;
    virtual void arbitrate() = 0;
    virtual bool completed(uint32_t entry) const = 0;
    virtual bool selected(uint32_t entry) const = 0;
    virtual RegisterVector take(uint32_t entry) = 0;
    virtual uint32_t occupancy() const = 0;
    virtual const WritebackStats& writeback_stats() const = 0;
};
}

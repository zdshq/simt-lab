#pragma once
#include <cstdint>

namespace simt::hw {
// Register and predicate values stay on circuit wires; the command contains only control metadata.
// 寄存器和谓词值留在电路连线上；命令只包含控制元数据。
struct CollectiveCommand {
    uint32_t collector = 0, completion = 0, opcode = 0, mode = 0;
    uint32_t ra = 0, rb = 0, mask = 0, members = 0, group = 0, warp = 0;
};
struct CollectiveStats { uint64_t clocks = 0, active_clocks = 0, accepted = 0, completed = 0, vote_reads = 0; };
class CollectiveExecution {
public:
    virtual ~CollectiveExecution() = default;
    virtual bool launch(const CollectiveCommand& command) = 0;
    virtual uint32_t occupancy() const = 0;
    virtual const CollectiveStats& collective_stats() const = 0;
};
}

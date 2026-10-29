#pragma once
#include "simt/register_file.hpp"
#include "simt/hardware_pipeline.hpp"
#include <span>
#include <vector>

namespace simt::hw {
struct CollectorGrant { uint32_t entry, source, reg, mask; };
struct CollectedOperand { uint32_t reg; RegisterVector values; };
struct CollectorStats {
    uint64_t clocks = 0, active_clocks = 0, allocated = 0, released = 0, reads = 0, bank_stalls = 0;
};

// Allocation, readiness, arbitration, and retained source data belong to the physical collector circuit.
// 分配、就绪、仲裁和保留的源数据均属于物理收集器电路。
class OperandCollector {
public:
    virtual ~OperandCollector() = default;
    virtual bool available() = 0;
    virtual uint32_t allocate(uint32_t warp, std::span<const RegisterUse> sources) = 0;
    virtual std::vector<CollectorGrant> collect() = 0;
    virtual bool ready(uint32_t entry) = 0;
    virtual std::vector<CollectedOperand> take(uint32_t entry) = 0;
    virtual uint32_t occupancy() const = 0;
    virtual uint32_t limit() const = 0;
    virtual const CollectorStats& collector_stats() const = 0;
};
std::unique_ptr<RegisterFile> make_rtl_operand_unit(uint32_t partitions, uint32_t banks, uint32_t collectors);
}

#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include "simt/hardware_memory.hpp"
#include "simt/instruction_memory.hpp"

namespace simt::hw {
struct InstructionFetchStats {
    uint64_t clocks=0, memory_requests=0, memory_responses=0, memory_stalls=0;
    std::array<uint64_t,8> queries{}, delivered{}, stalls{}, reads{}, hits{}, misses{}, fills{}, address_errors{};
};

// RTL refill handshakes use the same finite L2/DRAM hierarchy as data and copy traffic.
// RTL 补充握手使用与数据和拷贝流量相同的有限 L2/DRAM 层次。
class InstructionFetch {
public:
    virtual ~InstructionFetch() = default;
    virtual std::optional<InstructionWords> lookup(uint32_t partition,uint32_t pc) = 0;
    virtual void tick() = 0;
    virtual bool busy() const = 0;
    virtual const InstructionFetchStats& stats() const = 0;
};
std::unique_ptr<InstructionFetch> make_instruction_fetch(MemoryHierarchy& memory,const InstructionMapping& image,
    uint32_t sm,uint32_t partitions,uint64_t owner);
}

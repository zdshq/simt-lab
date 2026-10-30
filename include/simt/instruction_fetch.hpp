#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace simt::hw {
using InstructionWords=std::array<uint32_t,4>;
struct InstructionFetchStats {
    uint64_t clocks=0, memory_requests=0, memory_responses=0;
    std::array<uint64_t,8> queries{}, delivered{}, stalls{}, reads{}, hits{}, misses{}, fills{}, address_errors{};
};

// The cache/refill controller is RTL; its immutable external backing store is an explicit latency model.
// 缓存/补充控制器为 RTL；不可变的外部后备存储使用显式延迟模型。
class InstructionFetch {
public:
    virtual ~InstructionFetch() = default;
    virtual std::optional<InstructionWords> lookup(uint32_t partition,uint32_t pc) = 0;
    virtual void tick() = 0;
    virtual bool busy() const = 0;
    virtual const InstructionFetchStats& stats() const = 0;
};
std::unique_ptr<InstructionFetch> make_instruction_fetch(std::span<const InstructionWords> image,uint32_t partitions);
}

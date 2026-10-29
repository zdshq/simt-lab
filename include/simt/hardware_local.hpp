#pragma once
#include "simt/memory.hpp"
#include <span>
#include <utility>

namespace simt::hw {
// Local words interleave across lanes in a disjoint physical range above the global ISA window.
// local 字按 lane 交错，物理范围位于 global ISA 窗口之外。
class LocalAddressSpace final : public MemoryBacking {
public:
    static constexpr uint64_t base = uint64_t{1} << 32;
    static constexpr uint32_t thread_bytes = 16384, warp_bytes = thread_bytes * 32;
    explicit LocalAddressSpace(Memory& global) : global_(global) {}
    void attach(uint64_t warp, std::span<Memory> threads);
    void detach(uint64_t first, uint64_t count);
    static uint64_t translate(uint64_t warp, uint32_t lane, uint32_t offset);
    void check(uint64_t address, uint64_t size, uint32_t alignment = 1) const override;
    uint32_t read(uint64_t address, uint32_t size) const override;
    void write(uint64_t address, uint32_t size, uint32_t value) override;
private:
    Memory& global_;
    std::map<uint64_t, std::array<Memory*, 32>> warps_;
    std::pair<Memory*, uint32_t> resolve(uint64_t address) const;
};
}

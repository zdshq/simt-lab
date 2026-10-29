#include "simt/hardware_local.hpp"
#include <stdexcept>

namespace simt::hw {
// The owner retains architectural storage until all requests and inspection snapshots are finished.
// 所有请求及检查快照结束前，所有者持续保留架构存储。
void LocalAddressSpace::attach(uint32_t warp, std::span<Memory> threads) {
    if (threads.empty() || threads.size() > 32 || warps_.contains(warp))
        throw std::runtime_error("invalid_local_warp_registration");
    auto& mapped = warps_[warp];
    for (size_t lane = 0; lane < threads.size(); ++lane) mapped[lane] = &threads[lane];
}

uint64_t LocalAddressSpace::translate(uint32_t warp, uint32_t lane, uint32_t offset) {
    if (lane >= 32 || offset >= thread_bytes) throw std::runtime_error("invalid_local_translation");
    return base + uint64_t{warp} * warp_bytes + uint64_t{offset / 4} * 128 + lane * 4 + offset % 4;
}

// Sector fetches may include inactive lanes, but never cross an unregistered physical warp.
// sector 读取可能包含非活动 lane，但不能跨越未注册的物理 warp。
void LocalAddressSpace::check(uint64_t address, uint64_t size, uint32_t alignment) const {
    if (address < base) { global_.check(address, size, alignment); return; }
    if (!alignment || address % alignment) throw std::runtime_error("misaligned_memory_access");
    const auto relative = address - base, warp = relative / warp_bytes;
    if (warp > UINT32_MAX || !warps_.contains(static_cast<uint32_t>(warp)) || size > warp_bytes - relative % warp_bytes)
        throw std::runtime_error("unmapped_local_physical_address");
}

std::pair<Memory*, uint32_t> LocalAddressSpace::resolve(uint64_t address) const {
    const auto relative = address - base;
    const auto offset = static_cast<uint32_t>(relative % warp_bytes);
    return {warps_.at(static_cast<uint32_t>(relative / warp_bytes))[(offset / 4) % 32],
            (offset / 128) * 4 + offset % 4};
}

uint32_t LocalAddressSpace::read(uint64_t address, uint32_t size) const {
    if (address < base) return global_.read(address, size);
    if (size != 1 && size != 2 && size != 4) throw std::runtime_error("invalid_memory_width");
    check(address, size, size);
    const auto [memory, offset] = resolve(address);
    return memory ? memory->read(offset, size) : 0;
}

// Only actual DRAM write completion updates local architectural bytes; there is no shadow copy.
// 只有真实 DRAM 写完成才更新 local 架构字节，不维护影子副本。
void LocalAddressSpace::write(uint64_t address, uint32_t size, uint32_t value) {
    if (address < base) { global_.write(address, size, value); return; }
    if (size != 1 && size != 2 && size != 4) throw std::runtime_error("invalid_memory_width");
    check(address, size, size);
    const auto [memory, offset] = resolve(address);
    if (!memory) throw std::runtime_error("write_to_inactive_local_lane");
    memory->write(offset, size, value);
}
}

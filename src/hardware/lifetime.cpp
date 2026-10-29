#include "simt/hardware_memory.hpp"
#include <stdexcept>

namespace simt::hw {
// Range invalidation clears only matching sectors and preserves unrelated tags and outstanding fills.
// 范围失效仅清除匹配 sector，保留无关 tag 和在途 fill。
void SectorCache::invalidate_range(uint64_t address, uint64_t size) {
    if (!size) return;
    for (auto& line : lines_) {
        if (!line.tag) continue;
        const auto base = *line.tag * config_.line_bytes;
        for (uint32_t sector = 0; sector < line.valid.size(); ++sector) {
            const auto at = base + sector * sector_bytes;
            if (at < address + size && address < at + sector_bytes) line.valid[sector] = false;
        }
    }
}

// Allocation clearing cannot race an accepted access to the same physical sector.
// 分配清零不能与访问同一物理 sector 的已接受请求竞争。
void MemoryHierarchy::invalidate_range(uint64_t address, uint64_t size) {
    memory_.check(address, size);
    if (!size) return;
    const auto first = address / sector_bytes, last = (address + size - 1) / sector_bytes;
    for (const auto& [ticket, request] : requests_) {
        static_cast<void>(ticket);
        if (request.done) continue;
        for (uint32_t lane = 0; lane < 32; ++lane) {
            const auto sector = request.access.address[lane] / sector_bytes;
            if ((request.access.mask & (uint32_t{1} << lane)) && sector >= first && sector <= last)
                throw std::runtime_error("invalidate_busy_range");
        }
    }
    for (auto& cache : l1_) cache->invalidate_range(address, size);
    l2_.invalidate_range(address, size);
}

// Fault recovery consumes only the stopped owner's responses; accepted operations still finish normally.
// 故障恢复只消费已停止所有者的响应，已接受操作仍正常完成。
bool MemoryHierarchy::drain_owner(uint64_t owner) {
    bool complete = true;
    auto at = requests_.begin();
    while (at != requests_.end()) {
        if (at->second.owner != owner) { ++at; continue; }
        if (at->second.done) at = requests_.erase(at);
        else { complete = false; ++at; }
    }
    return complete;
}
}

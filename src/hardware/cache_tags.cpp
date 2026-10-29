#include "simt/hardware_memory.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt::hw {
// Geometry is validated before allocating tag, valid-bit, and byte-data arrays.
// 分配 tag、有效位和字节数据阵列之前检查几何参数。
SectorCache::SectorCache(SectorPort& lower, CacheConfig config) : lower_(lower), config_(config) {
    const uint64_t span = uint64_t{config.line_bytes} * config.ways;
    if (!span || !config.bytes || config.bytes % span || config.bytes > (64U << 20) ||
        config.line_bytes % sector_bytes || !config.banks || config.banks > 1024 ||
        !config.hit_latency || !config.miss_entries || !config.merge_limit || !config.capacity)
        throw std::runtime_error("invalid_cache_config");
    set_count_ = static_cast<uint32_t>(config.bytes / span);
    lines_.resize(config.bytes / config.line_bytes);
    bank_used_.resize(config.banks);
    fill_used_.resize(config.banks);
    for (auto& line : lines_) {
        line.valid.resize(config.line_bytes / sector_bytes);
        line.data.resize(config.line_bytes / sector_bytes);
    }
}

size_t SectorCache::sector_index(uint64_t address) const {
    return address % config_.line_bytes / sector_bytes;
}

// Full line tags retain all address bits not selected by the configurable set index.
// 完整行 tag 保留可配置组索引之外的全部地址位。
std::optional<size_t> SectorCache::find(uint64_t address) const {
    const auto tag = address / config_.line_bytes;
    const auto base = (tag % set_count_) * config_.ways;
    for (size_t slot = base; slot < base + config_.ways; ++slot)
        if (lines_[slot].tag == tag) return slot;
    return {};
}

// A pending fill locks its entire line against replacement, including other sectors.
// 待完成 fill 锁住整行，防止包含其他 sector 的行被替换。
std::optional<size_t> SectorCache::victim(uint64_t address) const {
    const auto base = (address / config_.line_bytes % set_count_) * config_.ways;
    std::optional<size_t> chosen;
    for (size_t slot = base; slot < base + config_.ways; ++slot) {
        const auto& line = lines_[slot];
        if (line.reservations) continue;
        if (!line.tag) return slot;
        if (!chosen || line.touched < lines_[*chosen].touched) chosen = slot;
    }
    return chosen;
}

void SectorCache::reserve_line(size_t slot, uint64_t address) {
    auto& line = lines_[slot];
    const auto tag = address / config_.line_bytes;
    if (line.tag != tag) {
        if (line.tag) ++stats_.evictions;
        line.tag = tag;
        std::fill(line.valid.begin(), line.valid.end(), false);
    }
    ++line.reservations;
    line.touched = ++age_;
}

// Explicit invalidation is legal only at a drained coherence boundary.
// 显式失效仅允许发生在事务已排空的一致性边界。
void SectorCache::invalidate() {
    if (outstanding() || !misses_.empty()) throw std::runtime_error("invalidate_busy_cache");
    for (auto& line : lines_) {
        line.tag.reset();
        std::fill(line.valid.begin(), line.valid.end(), false);
    }
}
}

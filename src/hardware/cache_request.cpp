#include "simt/hardware_memory.hpp"

namespace simt::hw {
// Banks arbitrate accepted requests; rejected requests retain no port or response ownership.
// bank 仲裁已接受请求，被拒绝请求不占用端口或响应资源。
bool SectorCache::available(const Packet& packet) {
    if (outstanding() == config_.capacity) { ++stats_.response_stalls; return false; }
    const auto bank = packet.address / sector_bytes % config_.banks;
    if (bank_used_[bank] == cycle_) { ++stats_.port_stalls; return false; }
    return true;
}

Ticket SectorCache::accept(const Packet& packet) {
    bank_used_[packet.address / sector_bytes % config_.banks] = cycle_;
    if (packet.write) ++stats_.writes; else ++stats_.reads;
    return reserve();
}

// Hits return bytes from this cache's SRAM model, not from the backing memory.
// 命中返回本级缓存 SRAM 模型中的字节，不读取底层内存。
Ticket SectorCache::hit(const Packet& packet, size_t slot) {
    auto& line = lines_[slot];
    const auto ticket = accept(packet);
    complete(ticket, line.data[sector_index(packet.address)], config_.hit_latency);
    line.touched = ++age_;
    ++stats_.hits;
    return ticket;
}

// Read misses share one downstream transaction; writes serialize with same-sector traffic.
// 读取未命中共享一个下游事务，写入与同 sector 流量串行化。
std::optional<Ticket> SectorCache::merge(const Packet& packet, MissEntry& entry) {
    if (packet.write || entry.packet.write || entry.waiters.size() == config_.merge_limit) {
        ++stats_.mshr_stalls;
        return {};
    }
    const auto ticket = accept(packet);
    entry.waiters.push_back(ticket);
    ++stats_.merges;
    return ticket;
}

// Write-through stores bypass allocation and invalidate their cached sector immediately.
// write-through 写入不分配缓存行，并立即使已缓存的对应 sector 失效。
std::optional<Ticket> SectorCache::miss(const Packet& packet, std::optional<size_t> slot) {
    if (misses_.size() == config_.miss_entries) { ++stats_.mshr_stalls; return {}; }
    const bool line_miss = !slot;
    if (!slot && !packet.write) slot = victim(packet.address);
    if (!slot && !packet.write) { ++stats_.reservation_stalls; return {}; }
    if (slot) {
        reserve_line(*slot, packet.address);
        if (packet.write) lines_[*slot].valid[sector_index(packet.address)] = false;
    }
    const auto ticket = accept(packet);
    misses_.emplace(packet.address, MissEntry{packet, slot, {}, {ticket}, cycle_ + config_.hit_latency, {}});
    if (!packet.write) {
        ++stats_.sector_misses;
        if (line_miss) ++stats_.line_misses;
    }
    return ticket;
}

std::optional<Ticket> SectorCache::submit(const Packet& packet) {
    check_packet(packet);
    if (!available(packet)) return {};
    const auto pending = misses_.find(packet.address);
    if (pending != misses_.end()) return merge(packet, pending->second);
    const auto slot = find(packet.address);
    if (!packet.write && slot && lines_[*slot].valid[sector_index(packet.address)]) return hit(packet, *slot);
    return miss(packet, slot);
}
}

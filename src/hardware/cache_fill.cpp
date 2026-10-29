#include "simt/hardware_memory.hpp"

namespace simt::hw {
// A fill sets only the requested sector valid, preserving other sectors and pending fills.
// fill 仅设置被请求 sector 的有效位，保留其他 sector 和待完成 fill。
void SectorCache::fill(MissEntry& entry, const Sector& data) {
    if (entry.slot) {
        auto& line = lines_[*entry.slot];
        --line.reservations;
        if (!entry.packet.write) {
            const auto sector = sector_index(entry.packet.address);
            line.data[sector] = data;
            line.valid[sector] = true;
            stats_.fill_bytes += sector_bytes;
        }
    }
    for (auto waiter : entry.waiters) complete(waiter, data, config_.hit_latency);
}

// Lower-level backpressure keeps the MSHR intact; a response releases every merged waiter.
// 下游反压时保留 MSHR，响应到达后释放所有合并等待者。
bool SectorCache::service(MissEntry& entry) {
    if (entry.eligible > cycle_) return false;
    if (!entry.downstream) {
        entry.downstream = lower_.submit(entry.packet);
        if (!entry.downstream) ++stats_.lower_stalls;
        return false;
    }
    if (!entry.returned) entry.returned = lower_.take(*entry.downstream);
    if (!entry.returned) return false;
    const auto bank = entry.packet.address / sector_bytes % config_.banks;
    if (fill_used_[bank] == cycle_) { ++stats_.fill_stalls; return false; }
    fill_used_[bank] = cycle_;
    fill(entry, *entry.returned);
    return true;
}

void SectorCache::tick() {
    ++cycle_;
    auto entry = misses_.begin();
    while (entry != misses_.end()) {
        if (service(entry->second)) entry = misses_.erase(entry);
        else ++entry;
    }
}
}

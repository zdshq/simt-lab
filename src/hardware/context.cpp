#include "simt/hardware_context.hpp"
#include <stdexcept>

namespace simt::hw {
HardwareContext::HardwareContext(Memory& global, HierarchyConfig config)
    : physical(global), memory(physical, config), sms_(config.sms) {}

uint64_t HardwareContext::new_owner() {
    if (next_owner_ == UINT64_MAX) throw std::runtime_error("hardware_owner_ids_exhausted");
    return next_owner_++;
}

// New launches receive distinct local physical tags, so retired cache lines cannot alias a later CTA.
// 新启动获得不同的 local 物理 tag，避免已退休缓存行与后续 CTA 别名。
uint64_t HardwareContext::reserve_local(uint64_t warps) {
    const auto limit = (UINT64_MAX - LocalAddressSpace::base) / LocalAddressSpace::warp_bytes;
    if (warps > limit - next_warp_) throw std::runtime_error("local_physical_ids_exhausted");
    const auto first = next_warp_; next_warp_ += warps;
    return first;
}

// Persistent memory counters include both engines; kernel issue/retire counters remain per launch.
// 持久内存计数包含两个引擎，kernel 发射/退休计数仍逐次启动统计。
void HardwareContext::dump(std::ostream& out) const {
    const auto& dram = memory.dram_stats(); const auto& l2 = memory.l2_stats();
    out << "{\"cycles\":" << cycle() << ",\"dram_reads\":" << dram.reads << ",\"dram_writes\":" << dram.writes
        << ",\"l2_hits\":" << l2.hits << ",\"l2_misses\":" << l2.sector_misses
        << ",\"cache_backend\":\"" << memory.l2_cache().name() << "\",\"l2_clocks\":" << memory.l2_cache().cycle()
        << ",\"l1\":[";
    for (uint32_t sm = 0; sm < sms_; ++sm) {
        if (sm) out << ',';
        const auto& stats = memory.l1_stats(sm);
        out << "{\"hits\":" << stats.hits << ",\"misses\":" << stats.sector_misses
            << ",\"backend\":\"" << memory.l1_cache(sm).name() << "\",\"clocks\":" << memory.l1_cache(sm).cycle() << '}';
    }
    out << "]}";
}
}

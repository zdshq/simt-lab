#include "simt/cache_engine.hpp"
#include <stdexcept>

namespace simt::hw {
bool cache_rtl_available() {
#ifdef SIMT_HAS_CACHE_RTL
    return true;
#else
    return false;
#endif
}

// Each fixed circuit has one request/SRAM port and finite physical miss/response storage.
// 每个固定电路具有一个请求/SRAM 端口及有限的物理未命中/响应存储。
CacheConfig rtl_cache_config(CacheLevel level) {
    return {level == CacheLevel::l1 ? 65536u : 262144u, 128,
            level == CacheLevel::l1 ? 4u : 8u, 1, 4, 4, 4, 8};
}

std::unique_ptr<CachePort> make_cache(SectorPort& lower, CacheConfig config, bool rtl, CacheLevel level) {
    if (!rtl) return std::make_unique<SectorCache>(lower, config);
#ifdef SIMT_HAS_CACHE_RTL
    return make_rtl_cache(lower, config, level);
#else
    static_cast<void>(level);
    throw std::runtime_error("cache_rtl_backend_unavailable; build with SIMT_CACHE_RTL=ON");
#endif
}
}

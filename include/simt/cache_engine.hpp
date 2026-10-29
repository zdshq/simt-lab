#pragma once
#include "simt/hardware_memory.hpp"

namespace simt::hw {
enum class CacheLevel { l1, l2 };
bool cache_rtl_available();
CacheConfig rtl_cache_config(CacheLevel level);
std::unique_ptr<CachePort> make_cache(SectorPort& lower, CacheConfig config, bool rtl, CacheLevel level);
std::unique_ptr<CachePort> make_rtl_cache(SectorPort& lower, CacheConfig config, CacheLevel level);
}

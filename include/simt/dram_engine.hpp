#pragma once
#include "simt/hardware_memory.hpp"

namespace simt::hw {
std::unique_ptr<DramPort> make_dram(MemoryBacking& memory,const DramConfig& config,bool rtl);
}

#include "simt/dram_engine.hpp"
#include <stdexcept>

namespace simt::hw {
#ifdef SIMT_HAS_REGISTER_RTL
std::unique_ptr<DramPort> make_rtl_dram(MemoryBacking& memory,const DramConfig& config);
#endif
std::unique_ptr<DramPort> make_dram(MemoryBacking& memory,const DramConfig& config,bool rtl) {
    if (!rtl) return std::make_unique<Dram>(memory,config);
#ifdef SIMT_HAS_REGISTER_RTL
    return make_rtl_dram(memory,config);
#else
    throw std::runtime_error("dram_rtl_backend_unavailable");
#endif
}
}

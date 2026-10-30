#include "simt/hardware_memory.hpp"
#include <stdexcept>

namespace simt::hw {
#ifdef SIMT_HAS_REGISTER_RTL
std::unique_ptr<SharedPort> make_rtl_shared_engine(SharedConfig config);
#endif
std::unique_ptr<SharedPort> make_shared_engine(SharedConfig config,bool rtl) {
    if (!rtl) return std::make_unique<BankedShared>(config);
#ifdef SIMT_HAS_REGISTER_RTL
    return make_rtl_shared_engine(config);
#else
    throw std::runtime_error("shared_rtl_backend_unavailable");
#endif
}
}

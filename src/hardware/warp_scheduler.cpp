#include "simt/warp_scheduler.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<WarpScheduler> make_warp_scheduler(uint32_t) { throw std::runtime_error("warp_scheduler_rtl_not_built"); }
}
#endif

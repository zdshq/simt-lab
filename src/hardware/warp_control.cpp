#include "simt/warp_control.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<WarpControl> make_warp_control() { throw std::runtime_error("warp_control_rtl_not_built"); }
}
#endif

#include "simt/warp_frontend.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<WarpFrontend> make_warp_frontend() { throw std::runtime_error("warp_frontend_rtl_not_built"); }
}
#endif

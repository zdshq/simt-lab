#include "simt/cta_barrier.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<CtaBarrierEngine> make_cta_barrier() { throw std::runtime_error("cta_barrier_rtl_not_built"); }
}
#endif

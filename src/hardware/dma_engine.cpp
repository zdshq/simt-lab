#include "simt/dma_engine.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<DmaEngine> make_dma_engine(uint32_t,uint32_t,uint32_t) { throw std::runtime_error("dma_rtl_not_built"); }
}
#endif

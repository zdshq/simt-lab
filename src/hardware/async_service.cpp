#include "simt/async_service.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<AsyncService> make_async_service(uint32_t,uint32_t) {
    throw std::runtime_error("async_service_rtl_not_built");
}
}
#endif

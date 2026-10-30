#include "simt/execution_arbiter.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<ExecutionArbiter> make_execution_arbiter(uint32_t,std::array<uint32_t,3>) {
    throw std::runtime_error("execution_arbiter_rtl_not_built");
}
}
#endif

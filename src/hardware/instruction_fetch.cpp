#include "simt/instruction_fetch.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<InstructionFetch> make_instruction_fetch(std::span<const InstructionWords>,uint32_t) {
    throw std::runtime_error("instruction_fetch_rtl_not_built");
}
}
#endif

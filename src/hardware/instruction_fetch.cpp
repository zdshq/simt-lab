#include "simt/instruction_fetch.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<InstructionFetch> make_instruction_fetch(MemoryHierarchy&,const InstructionMapping&,uint32_t,uint32_t,uint64_t) {
    throw std::runtime_error("instruction_fetch_rtl_not_built");
}
}
#endif

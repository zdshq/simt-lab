#include "simt/instruction_decoder.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<InstructionDecoder> make_instruction_decoder() { throw std::runtime_error("instruction_decoder_rtl_not_built"); }
}
#endif

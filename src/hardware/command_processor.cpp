#include "simt/command_processor.hpp"
#include <stdexcept>

namespace simt::hw {
#ifdef SIMT_HAS_REGISTER_RTL
std::unique_ptr<CommandProcessor> make_rtl_command_processor(uint32_t capacity);
#endif
std::unique_ptr<CommandProcessor> make_command_processor(uint32_t capacity) {
    if (!capacity || capacity>4096) throw std::runtime_error("invalid_command_processor_capacity");
#ifdef SIMT_HAS_REGISTER_RTL
    return make_rtl_command_processor(capacity);
#else
    throw std::runtime_error("command_processor_rtl_backend_unavailable");
#endif
}
}

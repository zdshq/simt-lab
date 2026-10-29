#include "simt/register_file.hpp"
#include "Vsimt_register4.h"
#include "Vsimt_register8.h"
#include "Vsimt_register16.h"
#include "Vsimt_register32.h"
#include "register_file_impl.hpp"

namespace simt::hw {
namespace {
using detail::RtlRegisterFile;
}

std::unique_ptr<RegisterFile> make_rtl_register_file(uint32_t partitions, uint32_t banks) {
    switch (partitions) {
    case 1: return std::make_unique<RtlRegisterFile<Vsimt_register32,32>>(banks);
    case 2: return std::make_unique<RtlRegisterFile<Vsimt_register16,16>>(banks);
    case 4: return std::make_unique<RtlRegisterFile<Vsimt_register8,8>>(banks);
    case 8: return std::make_unique<RtlRegisterFile<Vsimt_register4,4>>(banks);
    default: throw std::runtime_error("register_rtl_requires_power_of_two_partitions");
    }
}
}

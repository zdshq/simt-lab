#pragma once
#include <cstdint>

namespace simt {
uint32_t atomic_result(uint32_t mode, uint32_t old, uint32_t value, uint32_t replacement);
}

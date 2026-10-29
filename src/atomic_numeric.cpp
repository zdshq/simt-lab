#include "simt/atomic.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace simt {
// Pure integer ALU semantics are shared; each execution mode owns its memory ordering separately.
// 共享纯整数 ALU 语义，各执行模式分别负责内存顺序。
uint32_t atomic_result(uint32_t mode, uint32_t old, uint32_t value, uint32_t replacement) {
    switch (mode) {
    case 0: return old + value;
    case 1: return value;
    case 2: return old == value ? replacement : old;
    case 3: return old & value;
    case 4: return old | value;
    case 5: return old ^ value;
    case 6: return std::min(old, value);
    case 7: return std::max(old, value);
    case 8: return std::bit_cast<int32_t>(old) < std::bit_cast<int32_t>(value) ? old : value;
    case 9: return std::bit_cast<int32_t>(old) > std::bit_cast<int32_t>(value) ? old : value;
    default: throw std::runtime_error("invalid_atomic_mode");
    }
}
}

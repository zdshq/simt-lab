#include "simt/tensor.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace simt {
namespace {
// Binary16 values convert exactly to binary32, including subnormal values.
// binary16 数值可精确转换到 binary32，包括次正规数。
float half_float(uint32_t bits) {
    const auto sign = (bits & 0x8000) << 16;
    const auto exponent = (bits >> 10) & 31;
    const auto fraction = bits & 1023;
    if (exponent == 31) return to_float(sign | 0x7F800000 | (fraction << 13));
    if (exponent) return to_float(sign | ((exponent + 112) << 23) | (fraction << 13));
    const float magnitude = std::ldexp(static_cast<float>(fraction), -24);
    return bits & 0x8000 ? -magnitude : magnitude;
}
}

// Packed FP4 uses two elements per byte; all other formats have integral byte widths.
// 打包 FP4 每字节两个元素，其余格式具有整数个字节的宽度。
uint32_t tensor_element_size(uint32_t format) {
    if (format > 4) throw std::runtime_error("invalid_tensor_format");
    return format == 0 ? 4 : format < 3 ? 2 : 1;
}

// Decode source values independently of the host's native half-precision support.
// 解码源数值，不依赖宿主是否原生支持半精度。
float tensor_float(const Memory& memory, uint32_t base, uint64_t index, uint32_t format) {
    const auto size = tensor_element_size(format);
    const auto bits = memory.read(uint64_t{base} + (format == 4 ? index / 2 : index * size), size);
    if (format == 0) return to_float(bits);
    if (format == 1) return half_float(bits);
    if (format == 2) return to_float(bits << 16);
    if (format == 3) throw std::runtime_error("integer_tensor_on_float_path");
    constexpr std::array<float, 8> values{0.0F, 0.5F, 1.0F, 1.5F, 2.0F, 3.0F, 4.0F, 6.0F};
    const auto nibble = (bits >> ((index % 2) * 4)) & 15;
    return nibble & 8 ? -values[nibble & 7] : values[nibble & 7];
}

// INT8 sign interpretation is explicit and accumulation is handled as uint32.
// 显式解释 INT8 符号，累加由 uint32 处理。
int32_t tensor_integer(const Memory& memory, uint32_t base, uint64_t index) {
    const auto bits = static_cast<uint8_t>(memory.read(uint64_t{base} + index, 1));
    return std::bit_cast<int8_t>(bits);
}
}

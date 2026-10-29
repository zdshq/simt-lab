#include "bench.hpp"
#include "Vtensor_decode.h"
#include <array>
#include <bit>
#include <cmath>
#include <random>

namespace {
uint32_t canonical(uint32_t value) {
    return (value & 0x7f800000) == 0x7f800000 && (value & 0x7fffff) ? 0x7fc00000 : value;
}

// This reference uses exact host arithmetic rather than the RTL leading-bit normalization.
// 此参考使用精确宿主运算，不复用 RTL 的前导位规格化算法。
uint32_t half_reference(uint32_t bits) {
    const auto exponent = (bits >> 10)&31, fraction = bits&1023;
    if (exponent == 31) return fraction ? 0x7fc00000 : (bits&0x8000) << 16 | 0x7f800000;
    float value = std::ldexp(float(fraction+(exponent ? 1024 : 0)), exponent ? int(exponent)-25 : -24);
    if (bits&0x8000) value = -value;
    return std::bit_cast<uint32_t>(value);
}

void check(Vtensor_decode& dut, uint32_t format, uint32_t bits, bool high, uint32_t expected) {
    dut.format = format; dut.bits = bits; dut.high_nibble = high; dut.eval();
    require(dut.valid_format && dut.value == expected, "tensor format decode mismatch");
}

void exhaustive_half_and_bfloat() {
    Vtensor_decode dut;
    for (uint32_t bits = 0; bits < 65536; ++bits) {
        check(dut, 1, bits, false, half_reference(bits));
        check(dut, 2, bits, false, canonical(bits << 16));
    }
}

void packed_fp4_and_signed_integer() {
    Vtensor_decode dut;
    constexpr std::array<float, 8> values{0, .5F, 1, 1.5F, 2, 3, 4, 6};
    for (uint32_t bits = 0; bits < 256; ++bits) {
        check(dut, 3, bits, false, uint32_t(std::bit_cast<int8_t>(uint8_t(bits))));
        for (uint32_t high = 0; high < 2; ++high) {
            const auto nibble = (bits >> (high*4))&15;
            check(dut, 4, bits, high, std::bit_cast<uint32_t>(nibble&8 ? -values[nibble&7] : values[nibble&7]));
        }
    }
}

void float32_and_invalid_formats() {
    Vtensor_decode dut; std::mt19937 rng(7762);
    for (uint32_t trial = 0; trial < 20000; ++trial) { const uint32_t bits = rng(); check(dut, 0, bits, false, canonical(bits)); }
    for (uint32_t format = 5; format < 8; ++format) {
        dut.format = format; dut.eval(); require(!dut.valid_format && dut.value == 0, "invalid format accepted");
    }
}

void run_cases() {
    run_case("RTL tensor decode exhaustive FP16/BF16", exhaustive_half_and_bfloat);
    run_case("RTL tensor decode all packed FP4/INT8 bytes", packed_fp4_and_signed_integer);
    run_case("RTL tensor decode FP32/invalid format", float32_and_invalid_formats);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

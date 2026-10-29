#include "bench.hpp"
#include "Vfp32_fma.h"
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <deque>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace {
struct Operation { uint32_t a, b, c, expected; };

uint32_t reference(uint32_t a, uint32_t b, uint32_t c) {
    const auto value = std::fma(std::bit_cast<float>(a), std::bit_cast<float>(b), std::bit_cast<float>(c));
    return std::isnan(value) ? 0x7fc00000 : std::bit_cast<uint32_t>(value);
}

void compare(uint32_t actual, const Operation& operation) {
    if (actual == operation.expected) return;
    std::ostringstream message;
    message << std::hex << "FMA " << operation.a << " * " << operation.b << " + " << operation.c
            << ": expected " << operation.expected << ", got " << actual;
    throw std::runtime_error(message.str());
}

void drive(Vfp32_fma& dut, const Operation& operation) {
    dut.a = operation.a; dut.b = operation.b; dut.c = operation.c;
}

// Every accepted request is matched in order, including arbitrary stalls and bubbles.
// 每个已接受请求按序匹配，包含任意反压与输入气泡。
void stream(const std::vector<Operation>& operations, bool stalls) {
    Bench<Vfp32_fma> b(nullptr); std::mt19937 rng(98277);
    std::deque<Operation> pending;
    size_t submitted = 0, completed = 0; bool offered = false;
    while (completed < operations.size()) {
        require(b.cycle < operations.size()*10+100, "FMA stream failed to drain");
        offered = submitted < operations.size() && (offered || !stalls || rng()%4);
        b.dut.input_valid = offered; b.dut.output_ready = !stalls || rng()%3;
        if (offered) drive(b.dut, operations[submitted]);
        b.dut.eval();
        if (b.dut.output_valid && b.dut.output_ready) {
            require(!pending.empty(), "unsolicited FMA output");
            compare(b.dut.result, pending.front()); pending.pop_front(); ++completed;
        }
        if (offered && b.dut.input_ready) { pending.push_back(operations[submitted++]); offered = false; }
        b.tick();
    }
    require(submitted == operations.size() && pending.empty(), "FMA request accounting");
}

void corners_and_special_values() {
    constexpr std::array<uint32_t, 24> points{0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
        0x00800001, 0x3f000000, 0x3f800000, 0x3f800001, 0x3f7ffffe, 0xbf800000, 0x7f7fffff,
        0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0xffc00001, 0x33800000,
        0x3fc00000, 0x80800000, 0x00800002, 0x4b800001};
    std::vector<Operation> operations;
    for (auto a : points) for (auto b : points) for (auto c : points)
        operations.push_back({a, b, c, reference(a, b, c)});
    stream(operations, false);
}

void random_bits_and_cancellation() {
    std::mt19937 rng(391726);
    std::vector<Operation> operations;
    for (uint32_t index = 0; index < 250000; ++index) {
        const uint32_t a = rng(), b = rng(); uint32_t c = rng();
        if (index%3 == 0) c = reference(a, b, 0)^0x80000000;
        operations.push_back({a, b, c, reference(a, b, c)});
    }
    stream(operations, true);
}

void exact_rational_vectors() {
    std::ifstream input("reference.txt");
    require(bool(input), "missing independent rational vectors");
    std::vector<Operation> operations; Operation operation;
    while (input >> std::hex >> operation.a >> operation.b >> operation.c >> operation.expected)
        operations.push_back(operation);
    require(operations.size() == 12000, "incomplete rational vector set");
    stream(operations, true);
}

// Exact clock checks also ensure a held result cannot be overwritten by a new operation.
// 精确时钟检查同时确保尚未取走的结果不会被新运算覆盖。
void latency_stall_and_reset() {
    Bench<Vfp32_fma> b("fma-pipeline.vcd");
    b.dut.input_valid = 1; b.dut.output_ready = 0;
    drive(b.dut, {0x3f800001, 0x3f7ffffe, 0xbf800000, 0xa8800000});
    b.tick(); b.dut.input_valid = 0;
    for (uint32_t clock = 0; clock < 2; ++clock) { require(!b.dut.output_valid, "FMA early output"); b.tick(); }
    require(!b.dut.output_valid, "FMA skipped rounding register"); b.tick();
    require(b.dut.output_valid && b.dut.result == 0xa8800000, "FMA four-stage result");
    b.dut.input_valid = 1; drive(b.dut, {0, 0, 0, 0}); b.dut.eval();
    for (uint32_t clock = 0; clock < 20; ++clock) {
        require(!b.dut.input_ready && b.dut.output_valid && b.dut.result == 0xa8800000, "blocked FMA changed");
        b.tick();
    }
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.input_valid = 0; b.dut.output_ready = 1;
    for (uint32_t clock = 0; clock < 5; ++clock) { b.tick(); require(!b.dut.output_valid, "reset retained a result"); }
}

void run_cases() {
    require(std::fesetround(FE_TONEAREST) == 0, "cannot establish reference rounding mode");
    run_case("RTL FMA four-stage latency, backpressure and reset", latency_stall_and_reset);
    run_case("RTL FMA 13824 special/boundary combinations", corners_and_special_values);
    run_case("RTL FMA 250000 random/cancellation operations", random_bits_and_cancellation);
    run_case("RTL FMA 12000 exact rational vectors", exact_rational_vectors);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

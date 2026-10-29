#include "bench.hpp"
#include "Vtensor_pe.h"
#include <bit>
#include <cmath>

namespace {
void initialize(Bench<Vtensor_pe>& b, bool integer, bool scaled, uint32_t initial) {
    b.dut.initialize = 1; b.dut.initial_value = initial;
    b.dut.integer_mode = integer; b.dut.scaled = scaled; b.dut.input_valid = 0;
    b.tick(); b.dut.initialize = 0; b.dut.eval();
}

void dot(Bench<Vtensor_pe>& b, uint32_t a, uint32_t operand_b, uint32_t expected, uint32_t clocks = 4) {
    b.dut.a = a; b.dut.b = operand_b; b.dut.input_valid = 1; b.dut.eval();
    require(b.dut.input_ready, "PE failed to accept dot");
    b.tick(); b.dut.input_valid = 0;
    for (uint32_t clock = 0; clock < clocks; ++clock) b.tick();
    require(b.dut.accumulator == expected, "PE arithmetic mismatch");
}

void exhaustive_integer_multiplication() {
    Bench<Vtensor_pe> b(nullptr); initialize(b, true, false, 0xfffffff0);
    uint32_t expected = 0xfffffff0;
    b.dut.enabled = 1; b.dut.block_end = 0; b.dut.last = 0;
    for (uint32_t a = 0; a < 256; ++a) for (uint32_t operand_b = 0; operand_b < 256; ++operand_b) {
        expected += uint32_t(int32_t(std::bit_cast<int8_t>(uint8_t(a)))*std::bit_cast<int8_t>(uint8_t(operand_b)));
        dot(b, a, operand_b, expected);
    }
    b.dut.last = 1; dot(b, 0, 0, expected);
    require(b.dut.finished && !b.dut.input_ready, "PE final ownership");
}

void fused_sparse_and_scaled() {
    Bench<Vtensor_pe> b("tensor-pe.vcd"); initialize(b, false, false, 0xbf800000);
    b.dut.enabled = 1; b.dut.last = 1; dot(b, 0x3f800001, 0x3f7ffffe, 0xa8800000);
    initialize(b, false, false, 0x80000000); b.dut.enabled = 0;
    dot(b, 0x7f800000, 0x7fc00000, 0x80000000);
    initialize(b, false, true, 0x3f800000); b.dut.enabled = 1; b.dut.block_end = 1;
    b.dut.scale_a = 0x3f000000; b.dut.scale_b = 0x40000000;
    dot(b, 0x40000000, 0x40400000, 0x40e00000, 12);
    require(b.dut.finished, "scaled PE omitted completion");
}

// The final retiring token must not admit another operation through the feedback bypass.
// 最后一个令牌退休时，不能通过反馈旁路再接受一条运算。
void final_token_blocks_new_input() {
    Bench<Vtensor_pe> b("tensor-pe-last.vcd"); initialize(b, true, false, 1);
    b.dut.input_valid = 1; b.dut.a = 1; b.dut.b = 1; b.dut.enabled = 1; b.dut.last = 1;
    b.tick(); b.dut.a = 100; b.dut.last = 0; b.dut.eval();
    for (uint32_t clock = 0; clock < 10; ++clock) {
        require(!b.dut.input_ready, "final token accepted a successor before ownership release");
        b.tick();
    }
    require(b.dut.finished && b.dut.accumulator == 2, "successor overwrote final PE result");
}

void run_cases() {
    run_case("RTL PE all 65536 signed INT8 products and wrapping accumulator", exhaustive_integer_multiplication);
    run_case("RTL PE fused cancellation, sparse NaN skip, three-phase scaling", fused_sparse_and_scaled);
    run_case("RTL PE final token blocks feedback admission", final_token_blocks_new_input);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

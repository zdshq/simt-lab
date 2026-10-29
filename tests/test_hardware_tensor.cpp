#include "hardware_support.hpp"
#include "simt/hardware_tensor.hpp"
#include "simt/execution.hpp"
#include <bit>

namespace simt::hw::test {
// Hand-derived latency includes edge skew, one-hop transport, and final PE writeback.
// 手算延迟包含边缘错拍、逐跳传输和最终 PE 写回。
void rectangular_wavefront() {
    TensorArray array;
    array.begin(2, 3, true, false);
    TensorWave first, second;
    first.a = {2, 3}; first.b = {4, 5, 6};
    second.a = {7, 8}; second.b = {9, 10, 11}; second.last = true;
    require(array.push(first) && array.push(second), "input queue admission");
    for (uint32_t cycle = 0; cycle < 11; ++cycle) { array.tick(); require(!array.take(), "result before last PE"); }
    array.tick(); const auto result = array.take();
    require(result.has_value(), "missing wavefront completion");
    require((*result)[0] == 71 && (*result)[1] == 80 && (*result)[2] == 89, "first row products");
    require((*result)[4] == 84 && (*result)[5] == 95 && (*result)[6] == 106, "second row products");
    require(array.cycle() == 12 && array.stats().products == 12, "wavefront time or work");
}

void bounded_queue_and_response() {
    TensorConfig config; config.input_capacity = 1;
    TensorArray array(config); array.begin(1, 1, true, false);
    TensorWave wave; wave.a[0] = 2; wave.b[0] = 3;
    require(array.push(wave) && !array.push(wave), "input capacity ignored");
    array.tick(); wave.last = true; require(array.push(wave), "queue failed to release");
    for (uint32_t cycle = 0; cycle < 10; ++cycle) array.tick();
    rejects([&] { array.begin(1, 1, true, false); }, "tensor_array_busy");
    require(array.take()->at(0) == 12, "queued products lost");
    array.begin(1, 1, true, false);
}

void scaling_pipeline() {
    TensorArray array; std::array<uint32_t, 16> initial{}; initial[0] = float_bits(1);
    array.begin(1, 1, false, true, initial);
    TensorWave wave; wave.a[0] = float_bits(2); wave.b[0] = float_bits(3);
    wave.scale_a[0] = float_bits(.5F); wave.scale_b[0] = float_bits(2); wave.block_end = true;
    require(array.push(wave), "first scaled input");
    wave.a[0] = float_bits(4); wave.b[0] = float_bits(5);
    wave.scale_a[0] = float_bits(.25F); wave.scale_b[0] = float_bits(4); wave.last = true;
    require(array.push(wave), "second scaled input");
    for (uint32_t cycle = 0; cycle < 24; ++cycle) { array.tick(); require(!array.take(), "scale pipeline bypassed"); }
    array.tick(); require(array.take()->at(0) == float_bits(27), "scaled accumulation");
    require(array.stats().scale_products == 2 && array.stats().scale_accumulates == 2, "scale stages omitted");
}

void sparse_skip_and_integer_wrap() {
    TensorArray array; std::array<uint32_t, 16> initial{}; initial[0] = float_bits(-0.0F);
    array.begin(1, 1, false, false, initial);
    TensorWave wave; wave.enabled[0] = false; wave.b[0] = 0x7FC00000; wave.last = true;
    array.push(wave);
    for (uint32_t cycle = 0; cycle < 5; ++cycle) array.tick();
    require(array.take()->at(0) == float_bits(-0.0F), "absent product evaluated");
    initial[0] = 0xFFFFFFF0; array.begin(1, 1, true, false, initial);
    wave.enabled[0] = true; wave.a[0] = 127; wave.b[0] = 127; array.push(wave);
    for (uint32_t cycle = 0; cycle < 5; ++cycle) array.tick();
    require(array.take()->at(0) == 16113, "integer wrap changed");
}

// Input bubbles must delay completion without changing the arithmetic or retaining stale edge tokens.
// 输入气泡应延迟完成，但不能改变算术结果或保留过期边缘令牌。
void gaps_and_reuse() {
    TensorArray array;
    for (uint32_t trial = 0; trial < 3; ++trial) {
        array.begin(4, 4, true, false);
        TensorWave wave; wave.a.fill(2); wave.b.fill(3);
        array.push(wave);
        for (uint32_t cycle = 0; cycle < 30; ++cycle) { array.tick(); require(!array.take(), "unfinished stream completed"); }
        wave.last = true; array.push(wave);
        for (uint32_t cycle = 0; cycle < 11; ++cycle) array.tick();
        const auto result = array.take(); require(result.has_value(), "last token lost after gap");
        for (auto value : *result) require(value == 12, "stale accumulator or token");
    }
    require(array.stats().products == 96, "gap product count");
}

void fused_rounding() {
    TensorArray array; std::array<uint32_t, 16> initial{}; initial[0] = float_bits(-1);
    array.begin(1, 1, false, false, initial);
    TensorWave wave; wave.a[0] = 0x3F800001; wave.b[0] = 0x3F7FFFFE; wave.last = true;
    array.push(wave);
    for (uint32_t cycle = 0; cycle < 5; ++cycle) array.tick();
    require(array.take()->at(0) == 0xA8800000, "FMA product rounded before add");
}

int run_tests() {
    try {
        run("rectangular wavefront", rectangular_wavefront);
        run("bounded queue and response", bounded_queue_and_response);
        run("scaling pipeline", scaling_pipeline);
        run("sparse and integer", sparse_skip_and_integer_wrap);
        run("input gaps and array reuse", gaps_and_reuse);
        run("fused rounding", fused_rounding);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return simt::hw::test::run_tests(); }

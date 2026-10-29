#include "tensor_bench.hpp"

namespace {
Tile integer_rectangle() {
    Tile tile; tile.rows = 2; tile.columns = 3; tile.format = 3;
    Wave first, last;
    first.a = {2, 3}; first.b = {4, 5, 6};
    last.a = {7, 8}; last.b = {9, 10, 11}; last.last = true;
    tile.waves = {first, last};
    return tile;
}

void rectangular_clock_and_response() {
    TensorBench b("tensor-rectangle.vcd");
    require(b.run(integer_rectangle()) == 14, "RTL edge/FIFO/writeback latency differs");
    require(b.dut.result_values[0] == 71 && b.dut.result_values[6] == 106, "rectangle reference values");
    require(b.run(integer_rectangle(), true) >= 14, "input gaps completed too early");
}

void scaling_feedback_and_reuse() {
    TensorBench b("tensor-scaled.vcd");
    Tile tile; tile.rows = 1; tile.columns = 1; tile.scaled = true; tile.initial[0] = 0x3f800000;
    Wave first, last;
    first.a[0] = 0x40000000; first.b[0] = 0x40400000;
    first.scale_a[0] = 0x3f000000; first.scale_b[0] = 0x40000000; first.block_end = true;
    last.a[0] = 0x40800000; last.b[0] = 0x40a00000;
    last.scale_a[0] = 0x3e800000; last.scale_b[0] = 0x40800000; last.block_end = true; last.last = true;
    tile.waves = {first, last};
    require(b.run(tile) == 27, "three-phase reuse latency differs");
    require(b.dut.result_values[0] == 0x41d80000, "scaled two-block accumulation");
    b.run(integer_rectangle());
}

void sparse_nan_and_signed_zero() {
    TensorBench b("tensor-sparse.vcd");
    Tile tile; tile.rows = 4; tile.columns = 4;
    tile.initial.fill(0x80000000);
    Wave first, last; first.a.fill(0x7f800000); first.b.fill(0x7fc00000); first.enabled = 0;
    last = first; last.last = true; tile.waves = {first, last};
    b.run(tile);
    for (uint32_t cell = 0; cell < 16; ++cell) require(b.dut.result_values[cell] == 0x80000000, "sparse hole evaluated");
    tile.waves[0].enabled = 10; tile.waves[1].enabled = 5;
    b.run(tile);
}

Tile random_tile(std::mt19937& rng, uint32_t format, uint32_t length) {
    Tile tile; tile.rows = rng()%4+1; tile.columns = rng()%4+1; tile.format = format;
    tile.scaled = format != 3 && rng()%2;
    for (auto& value : tile.initial) value = rng();
    constexpr std::array<uint32_t, 6> scales{0, 0x80000000, 0x3e800000, 0x3f000000, 0x3f800000, 0x40000000};
    for (uint32_t k = 0; k < length; ++k) {
        Wave wave; wave.enabled = rng()%16; wave.high_a = rng()%16; wave.high_b = rng()%16;
        wave.block_end = (k+1)%4 == 0 || k+1 == length; wave.last = k+1 == length;
        for (uint32_t lane = 0; lane < 4; ++lane) {
            wave.a[lane] = rng(); wave.b[lane] = rng();
            wave.scale_a[lane] = scales[rng()%scales.size()]; wave.scale_b[lane] = scales[rng()%scales.size()];
        }
        tile.waves.push_back(wave);
    }
    return tile;
}

void all_formats_cpp_differential() {
    TensorBench b(nullptr); std::mt19937 rng(197701);
    for (uint32_t format = 0; format < 5; ++format)
        for (uint32_t trial = 0; trial < 20; ++trial) b.run(random_tile(rng, format, 1+rng()%32), true);
}

void maximum_k_and_fifo_backpressure() {
    TensorBench b(nullptr); std::mt19937 rng(8111);
    auto tile = random_tile(rng, 4, 256); tile.rows = 4; tile.columns = 4; tile.scaled = true;
    tile.initial.fill(0);
    b.run(tile);
    tile.format = 3; tile.scaled = false;
    require(b.run(tile) == 1033, "256-wave interval or tail latency differs");
    require(b.wave_stalls() > 0, "finite two-entry FIFO did not backpressure a saturated producer");
}

void command_rejection_and_reset() {
    TensorBench b("tensor-reset.vcd");
    b.dut.start_valid = 1; b.dut.start_rows = 0; b.dut.start_columns = 1; b.dut.eval();
    require(b.dut.command_error && !b.dut.start_ready, "invalid shape accepted"); b.tick();
    b.dut.start_rows = 1; b.dut.start_format = 3; b.dut.start_scaled = 1; b.dut.eval();
    require(b.dut.command_error && !b.dut.start_ready && !b.dut.busy, "scaled INT8 accepted");
    auto tile = integer_rectangle(); tile.format = 0; tile.scaled = true; b.start(tile);
    Wave wave; wave.last = true; b.drive(wave); b.dut.wave_valid = 1; b.dut.eval();
    require(b.dut.command_error && !b.dut.wave_ready, "scaled last wave omitted block end");
    wave.block_end = true; b.drive(wave); b.dut.eval(); require(b.dut.wave_ready, "rejected wave modified FIFO");
    b.tick(); b.dut.wave_valid = 0; b.tick(); b.tick();
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval();
    for (uint32_t clock = 0; clock < 20; ++clock) { b.tick(); require(!b.dut.busy && !b.dut.result_valid, "reset retained tile"); }
    b.run(integer_rectangle());
}

void run_cases() {
    run_case("RTL Tensor rectangular wavefront clocks and held response", rectangular_clock_and_response);
    run_case("RTL Tensor three-phase scaling feedback and mode reuse", scaling_feedback_and_reuse);
    run_case("RTL Tensor sparse NaN skip and signed-zero preservation", sparse_nan_and_signed_zero);
    run_case("RTL Tensor 100 mixed-format/gapped C++ differential tiles", all_formats_cpp_differential);
    run_case("RTL Tensor 256-wave full mesh and finite FIFO", maximum_k_and_fifo_backpressure);
    run_case("RTL Tensor command rejection and in-flight reset", command_rejection_and_reset);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

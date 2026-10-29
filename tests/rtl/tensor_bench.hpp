#pragma once
#include "bench.hpp"
#include "Vtensor_array.h"
#include "simt/hardware_tensor.hpp"
#include "simt/tensor.hpp"
#include <array>
#include <bit>
#include <optional>
#include <random>
#include <vector>

struct Wave {
    std::array<uint32_t, 4> a{}, b{}, scale_a{}, scale_b{};
    uint32_t enabled = 15, high_a = 0, high_b = 0;
    bool block_end = false, last = false;
};
struct Tile {
    uint32_t rows = 4, columns = 4, format = 0;
    bool scaled = false;
    std::array<uint32_t, 16> initial{};
    std::vector<Wave> waves;
};

inline uint32_t decoded(uint32_t value, uint32_t format, bool high) {
    if (format == 3) return uint32_t(std::bit_cast<int8_t>(uint8_t(value)));
    return simt::float_bits(simt::decode_tensor_float(value, high, format));
}

inline simt::hw::TensorWave reference_wave(const Wave& wave, uint32_t format) {
    simt::hw::TensorWave result;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        result.a[lane] = decoded(wave.a[lane], format, (wave.high_a >> lane)&1);
        result.b[lane] = decoded(wave.b[lane], format, (wave.high_b >> lane)&1);
        result.scale_a[lane] = wave.scale_a[lane]; result.scale_b[lane] = wave.scale_b[lane];
        result.enabled[lane] = (wave.enabled >> lane)&1;
    }
    result.block_end = wave.block_end; result.last = wave.last;
    return result;
}

class TensorBench : public Bench<Vtensor_array> {
public:
    explicit TensorBench(const char* path) : Bench(path) {}
    uint64_t wave_stalls() const { return wave_stalls_; }
    void start(const Tile& tile) {
        dut.start_valid = 1; dut.start_rows = tile.rows; dut.start_columns = tile.columns;
        dut.start_format = tile.format; dut.start_scaled = tile.scaled;
        for (uint32_t cell = 0; cell < 16; ++cell) dut.initial_values[cell] = tile.initial[cell];
        dut.result_ready = 0; dut.wave_valid = 0; dut.eval();
        require(dut.start_ready && !dut.command_error, "tile start rejected");
        tick(); dut.start_valid = 0; dut.eval();
    }
    void drive(const Wave& wave) {
        for (uint32_t lane = 0; lane < 4; ++lane) {
            dut.wave_a[lane] = wave.a[lane]; dut.wave_b[lane] = wave.b[lane];
            dut.wave_scale_a[lane] = wave.scale_a[lane]; dut.wave_scale_b[lane] = wave.scale_b[lane];
        }
        dut.wave_enabled = wave.enabled; dut.wave_high_a = wave.high_a; dut.wave_high_b = wave.high_b;
        dut.wave_block_end = wave.block_end; dut.wave_last = wave.last;
    }
    uint64_t run(const Tile& tile, bool gaps = false) {
        start(tile); wave_stalls_ = 0; simt::hw::TensorArray reference;
        reference.begin(tile.rows, tile.columns, tile.format == 3, tile.scaled, tile.initial);
        auto result = execute(tile, reference, gaps);
        check_result(result); hold_result(result);
        dut.result_ready = 1; tick(); dut.result_ready = 0;
        require(!dut.busy, "consumed tile retained ownership");
        return completion_clocks_;
    }
private:
    uint64_t completion_clocks_ = 0;
    uint64_t wave_stalls_ = 0;
    std::array<uint32_t, 16> execute(const Tile& tile, simt::hw::TensorArray& reference, bool gaps) {
        std::mt19937 rng(91013); std::optional<std::array<uint32_t, 16>> expected;
        size_t submitted = 0; bool offered = false; const auto begin = cycle;
        while (!dut.result_valid) {
            require(cycle-begin < 10000, "tensor tile failed to drain");
            offered = submitted < tile.waves.size() && (offered || !gaps || rng()%5 == 0);
            dut.wave_valid = offered;
            if (offered) drive(tile.waves[submitted]);
            dut.eval();
            require(!dut.command_error && !dut.transport_error, "tensor command or transport failure");
            if (offered && dut.wave_ready) {
                require(reference.push(reference_wave(tile.waves[submitted++], tile.format)), "reference FIFO fell behind RTL");
                offered = false;
            } else if (offered) ++wave_stalls_;
            tick(); reference.tick();
            if (auto result = reference.take()) expected = result;
        }
        completion_clocks_ = cycle-begin; dut.wave_valid = 0;
        require(submitted == tile.waves.size() && expected.has_value(), "premature tile completion");
        require(!dut.transport_error, "last PE transport failure");
        return *expected;
    }
    void check_result(const std::array<uint32_t, 16>& expected) {
        for (uint32_t cell = 0; cell < 16; ++cell) {
            if (dut.result_values[cell] == expected[cell]) continue;
            throw std::runtime_error("tensor cell "+std::to_string(cell)+" differs from C++ array: expected "+
                                     std::to_string(expected[cell])+", got "+std::to_string(dut.result_values[cell]));
        }
    }
    // Backpressure keeps every result bit and ownership stable, even when another start is presented.
    // 即使有新的启动请求，反压仍保持所有结果位和资源归属稳定。
    void hold_result(const std::array<uint32_t, 16>& expected) {
        dut.start_valid = 1; dut.eval();
        for (uint32_t clock = 0; clock < 9; ++clock) {
            require(dut.result_valid && !dut.start_ready && !dut.wave_ready, "held tile lost ownership");
            check_result(expected); tick();
        }
        dut.start_valid = 0;
    }
};

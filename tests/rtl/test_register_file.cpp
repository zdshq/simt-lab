#include "bench.hpp"
#include "Vregister_file.h"
#include <array>
#include <bit>
#include <random>
#include <vector>

namespace {
using Vector = std::array<uint32_t,32>;
using Reference = std::vector<std::array<Vector,64>>;
class TestBench : public Bench<Vregister_file> {
public:
    explicit TestBench(const char* path) : Bench(path) { dut.port_groups = 4; dut.eval(); }
};

// Pack rows independently one bit at a time, including geometries crossing a 32-bit boundary.
// 独立逐位打包行地址，包含跨 32 位边界的几何配置。
uint64_t row_bits(uint64_t previous, uint32_t slots, uint32_t bank, uint32_t slot, uint32_t reg) {
    const auto width = std::countr_zero(slots)+4;
    for (uint32_t bit = 0; bit < width; ++bit) {
        const auto flag = uint64_t{1} << (bank*width+bit);
        previous = (previous & ~flag) | (((slot*16+reg/4) >> bit & 1) ? flag : 0);
    }
    return previous;
}

void write(TestBench& b, uint32_t slot, uint32_t reg, uint32_t mask, const Vector& values) {
    const auto bank = reg%4;
    b.dut.write_rows = row_bits(b.dut.write_rows, b.dut.slot_count, bank, slot, reg);
    b.dut.write_masks[bank] = mask;
    for (uint32_t lane = 0; lane < 32; ++lane) b.dut.write_data[bank*32+lane] = values[lane];
    b.dut.write_valid |= 1u << bank; b.dut.eval();
}

Vector read(TestBench& b, uint32_t slot, uint32_t reg) {
    const auto bank = reg%4;
    b.dut.read_rows = row_bits(b.dut.read_rows, b.dut.slot_count, bank, slot, reg);
    b.dut.read_valid |= 1u << bank; b.dut.eval();
    Vector result;
    for (uint32_t lane = 0; lane < 32; ++lane) result[lane] = b.dut.read_data[bank*32+lane];
    return result;
}

void commit(TestBench& b) {
    b.tick(); b.dut.read_valid = b.dut.write_valid = b.dut.clear_slots = 0; b.dut.eval();
}

// Pin withdrawal proves combinational bypass did not secretly write the storage array.
// 撤销引脚证明组合旁路没有偷偷写入存储阵列。
void masks_bypass_and_edges() {
    TestBench b("register-bypass.vcd"); Vector first, next;
    first.fill(0x12345678); next.fill(0xabcdef01);
    require(!b.dut.clock_enable, "idle clock gate opened");
    write(b, 0, 4, UINT32_MAX, first);
    require(read(b, 0, 4) == first && b.dut.clock_enable, "write bypass or gate absent");
    b.dut.write_valid = 0; b.dut.eval();
    require(read(b, 0, 4) == Vector{}, "bypass changed storage without edge");
    b.dut.write_valid = 1; commit(b);
    write(b, 0, 4, 0x80000001, next);
    auto expected = first; expected[0] = expected[31] = next[0];
    require(read(b, 0, 4) == expected, "masked same-cycle bypass differs"); commit(b);
    require(read(b, 0, 4) == expected, "edge lost partial write");
}

void zero_reset_and_slot_reuse() {
    TestBench b("register-reuse.vcd"); Vector values; values.fill(87);
    write(b, 0, 7, UINT32_MAX, values); write(b, 1, 4, UINT32_MAX, values); commit(b);
    write(b, 0, 63, UINT32_MAX, values);
    require(read(b, 0, 63) == Vector{}, "hardwired R63 is writable"); commit(b);
    b.dut.clear_slots = 1; b.dut.eval();
    require(read(b, 0, 7) == Vector{}, "reuse leaked prior owner");
    require(read(b, 1, 4) == values, "reuse cleared a different owner"); commit(b);
    write(b, 0, 7, 1, values); commit(b);
    Vector expected{}; expected[0] = 87;
    require(read(b, 0, 7) == expected, "partial write revived stale lanes");
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.read_valid = b.dut.write_valid = 0; b.dut.eval();
    require(read(b, 1, 4) == Vector{}, "reset leaked data");
}

void port_capacity_and_independent_directions() {
    TestBench b("register-ports.vcd"); Vector values; values.fill(11);
    for (uint32_t bank = 0; bank < 4; ++bank) {
        b.dut.query_register = bank; b.dut.eval();
        require(b.dut.read_available && b.dut.write_available, "empty bank unavailable");
        write(b, b.dut.slot_count-1, bank, UINT32_MAX, values);
        require(b.dut.read_available && !b.dut.write_available, "read/write directions coupled");
        require(read(b, b.dut.slot_count-1, bank) == values, "last slot lost bank data");
        require(!b.dut.read_available, "occupied read bank available");
    }
    b.dut.query_register = 63; b.dut.eval();
    require(b.dut.read_available && b.dut.write_available, "R63 consumes port"); commit(b);
    b.dut.query_register = 0; b.dut.eval();
    require(b.dut.read_available && b.dut.write_available, "next clock failed to release ports");
    read(b, 0, 0); write(b, 0, 0, 1, values);
    b.dut.query_register = 2; b.dut.port_groups = 2; b.dut.eval();
    require(!b.dut.read_available && !b.dut.write_available, "two-group conflict bypassed");
    b.dut.query_register = 1; b.dut.eval();
    require(b.dut.read_available && b.dut.write_available, "independent group blocked");
    b.dut.port_groups = 1; b.dut.eval();
    require(!b.dut.read_available && !b.dut.write_available, "single-group conflict bypassed");
}

// Compare actual bits against an independent array across masked writes, slot clears, and bank addresses.
// 在掩码写入、槽清零和 bank 地址组合中，将实际位值与独立数组对照。
void randomized_data_path() {
    TestBench b(nullptr); Reference reference(b.dut.slot_count); std::mt19937 random(20261030);
    for (uint32_t cycle = 0; cycle < 600; ++cycle) {
        if (cycle%11 == 0) {
            const auto slot = random()%b.dut.slot_count;
            b.dut.clear_slots = 1u << slot; reference[slot] = {};
        }
        for (uint32_t bank = 0; bank < 4; ++bank) {
            const auto slot = random()%b.dut.slot_count, reg = (random()%16)*4+bank, mask = random();
            Vector values; for (auto& value : values) value = random();
            write(b, slot, reg, mask, values);
            if (reg != 63) for (uint32_t lane = 0; lane < 32; ++lane)
                if (mask & (1u << lane)) reference[slot][reg][lane] = values[lane];
            require(read(b, slot, reg) == reference[slot][reg], "random forwarded vector differs");
        }
        commit(b);
        for (uint32_t bank = 0; bank < 4; ++bank) {
            const auto slot = random()%b.dut.slot_count, reg = (random()%16)*4+bank;
            require(read(b, slot, reg) == reference[slot][reg], "random committed vector differs");
        }
        commit(b);
    }
}

void run_cases() {
    run_case("register masked bypass and edge ownership", masks_bypass_and_edges);
    run_case("register zero reset and physical slot reuse", zero_reset_and_slot_reuse);
    run_case("register port capacity and independent directions", port_capacity_and_independent_directions);
    run_case("register 2400-write independent data differential", randomized_data_path);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

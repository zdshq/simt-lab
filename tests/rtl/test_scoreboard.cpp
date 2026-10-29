#include "bench.hpp"
#include "Vlane_scoreboard.h"
#include "simt/hardware_pipeline.hpp"
#include <random>

namespace {
using Model = simt::hw::Scoreboard;

void apply(Model& model, uint32_t command, uint32_t reg, uint32_t mask) {
    switch (command) {
    case 0: model.reserve_read(reg, mask); break;
    case 1: model.collect_read(reg, mask); break;
    case 2: model.reserve_write(reg, mask); break;
    case 3: model.retire_write(reg, mask); break;
    case 4: model.reserve_predicate(reg, mask); break;
    case 5: model.retire_predicate(reg, mask); break;
    default: throw std::runtime_error("unknown operation");
    }
}

void compare_queries(Vlane_scoreboard& dut, const Model& model, uint32_t reg, uint32_t mask) {
    dut.query_register = reg; dut.query_predicate = reg%8; dut.query_mask = mask; dut.eval();
    require(bool(dut.readable) == model.readable(reg, mask), "RAW differs from C++");
    require(bool(dut.writable) == model.writable(reg, mask), "WAR/WAW differs from C++");
    require(bool(dut.predicate_ready) == model.predicate_ready(reg%8, mask), "predicate differs from C++");
}

// Rejected commands must leave every reservation unchanged, including partially invalid masks.
// 被拒绝命令必须保留所有预约，包括掩码中只有部分 lane 非法的情况。
void command(Bench<Vlane_scoreboard>& b, Model& model, uint32_t op, uint32_t reg, uint32_t mask) {
    b.dut.command_valid = 1; b.dut.command = op;
    b.dut.command_register = reg; b.dut.command_mask = mask; b.dut.eval();
    Model next = model;
    bool valid = true;
    try { apply(next, op, reg, mask); } catch (const std::exception&) { valid = false; }
    require(bool(b.dut.command_error) == !valid, "transition legality differs from C++");
    if (valid && b.dut.command_ready) model = next;
    b.tick(); b.dut.command_valid = 0;
    compare_queries(b.dut, model, reg, mask);
}

void lane_masks_rmw_and_predicates() {
    Bench<Vlane_scoreboard> b("scoreboard-contract.vcd"); Model model;
    command(b, model, 2, 3, 0xFF); compare_queries(b.dut, model, 3, 0x100);
    command(b, model, 2, 3, 0x100); command(b, model, 2, 3, 1);
    command(b, model, 3, 3, 0xFF); command(b, model, 3, 3, 0x100);
    command(b, model, 0, 4, 3); command(b, model, 0, 4, 1);
    command(b, model, 1, 4, 3); compare_queries(b.dut, model, 4, 2);
    command(b, model, 1, 4, 3); command(b, model, 1, 4, 1);
    command(b, model, 0, 5, 1); command(b, model, 2, 5, 1);
    command(b, model, 1, 5, 1); command(b, model, 3, 5, 1);
    for (uint32_t op = 0; op < 4; ++op) command(b, model, op, 63, UINT32_MAX);
    command(b, model, 4, 2, 7); compare_queries(b.dut, model, 2, 8);
    command(b, model, 4, 2, 1); command(b, model, 5, 2, 7);
}

void bounded_reader_counter() {
    Bench<Vlane_scoreboard> b("scoreboard-capacity.vcd"); Model model;
    for (uint32_t read = 0; read < 7; ++read) command(b, model, 0, 9, 1);
    b.dut.command_valid = 1; b.dut.command = 0; b.dut.command_register = 9; b.dut.command_mask = 1; b.dut.eval();
    require(!b.dut.command_ready && !b.dut.command_error, "saturation must stall without overflow");
    for (uint32_t cycle = 0; cycle < 4; ++cycle) b.tick();
    for (uint32_t read = 0; read < 7; ++read) command(b, model, 1, 9, 1);
    require(b.dut.writable, "stalled reader incorrectly incremented counter");
}

void randomized_cpp_differential() {
    Bench<Vlane_scoreboard> b("scoreboard-differential.vcd"); Model model;
    std::mt19937 rng(5723);
    for (uint32_t step = 0; step < 5000; ++step) {
        const auto op = rng()%6, reg = op >= 4 ? rng()%8 : rng()%64;
        const uint32_t mask = uint32_t{1} << (rng()%32);
        command(b, model, op, reg, mask);
        compare_queries(b.dut, model, rng()%64, rng());
    }
}

void run_cases() {
    run_case("RTL scoreboard masks/RMW/predicate C++ agreement", lane_masks_rmw_and_predicates);
    run_case("RTL scoreboard finite reader saturation", bounded_reader_counter);
    run_case("RTL scoreboard 5000-command C++ differential", randomized_cpp_differential);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

#include "bench.hpp"
#include "Vcollective_execute.h"
#include "simt/control.hpp"
#include <bit>
#include <optional>
#include <random>

namespace {
using Vector = std::array<uint32_t,32>;
using TestBench = Bench<Vcollective_execute>;
struct Command {
    uint32_t opcode = 32, mode = 0, mask = UINT32_MAX, members = UINT32_MAX, group = UINT32_MAX;
    uint32_t predicates = 0xaaaaaaaa, ra = 1, rb = 2, count = 2, entry = 0;
    std::array<Vector,2> data{};
};
void drive(TestBench& b, const Command& c) {
    auto& d = b.dut;
    d.input_valid = 1; d.opcode = c.opcode; d.mode = c.mode; d.mask = c.mask;
    d.members = c.members; d.group_mask = c.group; d.predicates = c.predicates;
    d.ra = c.ra; d.rb = c.rb; d.source_count = c.count; d.source_registers = 1 | (2<<6);
    d.input_entry = c.entry;
    for (uint32_t source = 0; source < 2; ++source)
        for (uint32_t lane = 0; lane < 32; ++lane) d.source_data[source*32+lane] = c.data[source][lane];
    d.eval();
}

// Functional ISA execution is independent of the RTL's mux topology and elastic storage.
// 功能 ISA 执行独立于 RTL 的多路器结构和可反压存储。
Vector reference(const Command& c) {
    simt::Instruction i; i.op = static_cast<simt::Op>(c.opcode); i.mode = c.mode; i.imm = c.members;
    i.ra = c.ra; i.rb = c.rb; i.rd = 4;
    simt::Warp before, after; simt::Cta cta; simt::Config config; simt::Memory memory;
    before.r[1] = c.data[0]; before.r[2] = c.data[1];
    if (c.ra < 8) before.predicates[c.ra] = c.predicates;
    const simt::ExecutionContext context{i,cta,before,after,config,0,memory,memory};
    require(simt::execute_collective(context,{0,c.group},c.mask),"reference opcode unsupported");
    if (c.opcode != 30) return after.r[4];
    Vector value{}; value[0] = after.predicates[4]; return value;
}
void check(TestBench& b, const Command& c) {
    require(b.dut.output_valid && b.dut.output_entry == c.entry,"collective identity differs");
    const auto expected = reference(c);
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(b.dut.output_data[lane] == expected[lane],"collective lane result differs");
}
Command random_command(std::mt19937& rng) {
    Command c; c.opcode = 30+rng()%4; c.mode = rng()%4; c.entry = rng()%32;
    c.mask = rng(); c.members = (rng() & c.mask); if (!c.members) c.members = c.mask & -c.mask;
    c.group = c.mask | rng(); c.predicates = rng();
    for (uint32_t lane = 0; lane < 32; ++lane) {
        c.data[0][lane] = rng(); c.data[1][lane] = rng()%32;
        if (!(c.members & (1u<<c.data[1][lane]))) c.data[1][lane] = std::countr_zero(c.members);
    }
    return c;
}
void execute(TestBench& b, const Command& c) {
    drive(b,c); require(b.dut.input_accept && !b.dut.input_error,"valid collective rejected"); b.tick();
    b.dut.input_valid = 0; b.dut.eval(); check(b,c);
    b.dut.output_ready = 1; b.tick(); b.dut.output_ready = 0; b.dut.eval();
    require(!b.dut.output_valid,"collective result leaked");
}
void modes_masks_and_cross_lane_values() {
    TestBench b(nullptr); std::mt19937 rng(39402);
    for (uint32_t step = 0; step < 3000; ++step) execute(b,random_command(rng));
    Command c;
    for (uint32_t lane = 0; lane < 32; ++lane) { c.data[0][lane] = 100+lane; c.data[1][lane] = 31-lane; }
    execute(b,c); c.rb = 1; c.data[0].fill(7); execute(b,c);
    c.ra = c.rb = 63; c.count = 0; execute(b,c);
    c.opcode = 31; c.ra = 7; c.predicates = UINT32_MAX; execute(b,c);
    for (uint32_t op = 30; op <= 33; ++op) {
        c.opcode = op; c.mask = c.members = 0; c.mode = 255; c.ra = 63; execute(b,c);
    }
}
void faults_do_not_capture_partial_results() {
    TestBench b("collective-faults.vcd"); Command c;
    const auto reject = [&](const Command& input, uint32_t error) {
        drive(b,input); require(b.dut.input_error == error && !b.dut.input_accept,"collective error mismatch");
        b.tick(); require(!b.dut.output_valid,"fault captured a result");
    };
    c.opcode = 99; reject(c,1); c.opcode = 30; c.members = 0; reject(c,2);
    c.members = 3; c.mask = 1; reject(c,3);
    c.mask = UINT32_MAX; c.opcode = 31; c.ra = 8; reject(c,4);
    c.ra = 1; c.mode = 4; reject(c,5);
    c.opcode = 32; c.mode = 0; c.count = 0; reject(c,7); c.count = 2;
    c.data[1][0] = 32; reject(c,6); c.data[1][0] = UINT32_MAX; reject(c,6);
    c.data[1][0] = 2; reject(c,6); c.data[1][0] = 0; c.data[1][31] = UINT32_MAX; execute(b,c);
}
void hold_replace_and_reset() {
    TestBench b("collective-hold.vcd"); std::mt19937 rng(5941);
    const auto first = random_command(rng); drive(b,first); b.tick();
    for (uint32_t wait = 0; wait < 12; ++wait) {
        drive(b,random_command(rng)); require(!b.dut.input_accept,"full elastic register accepted input");
        check(b,first); b.tick();
    }
    const auto next = random_command(rng); drive(b,next); b.dut.output_ready = 1; b.dut.eval();
    require(b.dut.input_accept,"same-edge replacement refused"); check(b,first); b.tick();
    b.dut.output_ready = 0; b.dut.input_valid = 0; b.dut.eval(); check(b,next);
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval();
    require(!b.dut.output_valid && !b.dut.clock_enable,"reset failed to drain collective"); execute(b,first);
}
void randomized_backpressure() {
    TestBench b(nullptr); std::mt19937 rng(65930); std::optional<Command> pending;
    for (uint32_t step = 0; step < 5000; ++step) {
        const auto c = random_command(rng); drive(b,c);
        b.dut.input_valid = rng()%3 != 0; b.dut.output_ready = rng()%4 != 0; b.dut.eval();
        require(bool(b.dut.output_valid) == pending.has_value(),"elastic valid differs");
        if (pending) check(b,*pending);
        const bool pop = pending && b.dut.output_ready;
        const bool push = b.dut.input_valid && (!pending || pop);
        require(bool(b.dut.input_accept) == push,"elastic handshake differs");
        if (pop) pending.reset(); if (push) pending = c; b.tick();
    }
    b.dut.input_valid = 0; b.dut.output_ready = 1; b.dut.eval();
    if (pending) check(b,*pending); b.tick(); require(!b.dut.output_valid,"stream failed to drain");
}
void run_cases() {
    run_case("collective ISA differential masks/modes/crossbar/constant sources",modes_masks_and_cross_lane_values);
    run_case("collective descriptor failures preserve state",faults_do_not_capture_partial_results);
    run_case("collective hold, simultaneous pop/push and reset",hold_replace_and_reset);
    run_case("collective independent randomized elastic reference",randomized_backpressure);
}
int application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return application(); }

#include "bench.hpp"
#include "Vinteger_execute.h"
#include "simt/execution.hpp"
#include "simt/memsync.hpp"
#include <deque>
#include <random>

namespace {
using Vector = std::array<uint32_t,32>;
struct Command {
    uint32_t opcode = 7, mode = 0, mask = UINT32_MAX, immediate = 0;
    uint32_t ra = 1, rb = 2, rc = 3, count = 3, entry = 0, latency = 1;
    std::array<Vector,3> data{};
};
using TestBench = Bench<Vinteger_execute>;

void drive(TestBench& b, const Command& c) {
    auto& d = b.dut;
    d.input_valid = 1; d.opcode = c.opcode; d.mode = c.mode; d.mask = c.mask; d.immediate = c.immediate;
    d.ra = c.ra; d.rb = c.rb; d.rc = c.rc; d.source_count = c.count; d.source_registers = 1 | (2<<6) | (3<<12);
    d.input_entry = c.entry; d.latency = c.latency;
    d.warp_id = 11; d.block_id = 9; d.sm_id = 1; d.threads = 1000; d.blocks = 17;
    for (uint32_t source = 0; source < 3; ++source)
        for (uint32_t lane = 0; lane < 32; ++lane) d.source_data[source*32+lane] = c.data[source][lane];
    d.eval();
}

// The existing functional ISA supplies an independent executable reference for arithmetic and faults.
// 既有功能 ISA 为算术和错误提供独立的可执行参考。
Vector reference(const Command& c) {
    simt::Instruction instruction;
    instruction.op = static_cast<simt::Op>(c.opcode); instruction.mode = c.mode; instruction.imm = c.immediate;
    instruction.ra = c.ra; instruction.rb = c.rb; instruction.rc = c.rc; instruction.rd = 4;
    simt::Warp before, after; simt::Cta cta; simt::Config config; simt::Memory memory;
    cta.block_id = 9; cta.sm_id = 1; config.threads = 1000; config.blocks = 17;
    for (uint32_t source = 0; source < c.count; ++source) before.r[source+1] = c.data[source];
    const simt::ExecutionContext context{instruction,cta,before,after,config,11,memory,memory};
    if (instruction.op == simt::Op::CVTA) simt::execute_memory(context,c.mask);
    else for (uint32_t lane = 0; lane < 32; ++lane)
        if (c.mask & (1u<<lane)) simt::execute_scalar_lane(context,lane);
    if (instruction.op != simt::Op::ISETP) return after.r[4];
    Vector result{}; result[0] = after.predicates[4]; return result;
}

void check_output(TestBench& b, uint32_t entry, const Vector& expected) {
    require(b.dut.output_valid && b.dut.output_entry == entry,"integer completion identity/valid differs");
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(b.dut.output_data[lane] == expected[lane],"integer result differs at lane "+std::to_string(lane));
}

void execute(TestBench& b, const Command& c) {
    const auto expected = reference(c); drive(b,c);
    require(b.dut.input_accept && !b.dut.input_error,"legal integer input rejected"); b.tick();
    b.dut.input_valid = 0; b.dut.eval();
    for (uint32_t step = 1; step < c.latency; ++step) {
        require(!b.dut.output_valid,"integer latency completed early"); b.tick();
    }
    check_output(b,c.entry,expected); b.dut.output_ready = 1; b.tick();
    b.dut.output_ready = 0; b.dut.eval(); require(!b.dut.occupancy,"integer result slot leaked");
}

void arithmetic_modes_and_special_values() {
    TestBench b(nullptr); Command c; std::mt19937 random(8041);
    constexpr uint32_t edges[] = {0,1,31,32,63,0x7fffffff,0x80000000,UINT32_MAX};
    for (uint32_t op = 0; op <= 14; ++op)
        for (uint32_t sample = 0; sample < 128; ++sample) {
            c.opcode = op; c.mode = sample%6; c.immediate = op == 3 ? sample%7 : uint32_t(random());
            c.mask = sample%4 == 0 ? 0 : sample%4 == 1 ? UINT32_MAX : uint32_t(random());
            for (auto& source : c.data) for (auto& word : source)
                word = sample < 8 ? edges[(sample+uint32_t(random()))%8] : uint32_t(random());
            execute(b,c);
        }
    c.opcode = 7; c.mask = UINT32_MAX; c.ra = c.rb = 1; c.rc = 63; c.count = 1;
    execute(b,c); c.ra = c.rb = 63; c.count = 0; execute(b,c);
}

void address_conversion_and_faults() {
    TestBench b("integer-faults.vcd"); Command c; c.opcode = 43;
    for (uint32_t mode = 0; mode < 4; ++mode) {
        c.mode = mode;
        const uint32_t limit = mode%2 ? 16384 : 65536, base = mode%2 ? 0xf1000000 : 0xf0000000;
        for (uint32_t lane = 0; lane < 32; ++lane) c.data[0][lane] = (lane%2 ? limit-1 : 0)+(mode >= 2 ? base : 0);
        execute(b,c);
        c.data[0][7] = mode >= 2 ? base-1 : limit; drive(b,c);
        require(b.dut.input_error == 5 && !b.dut.input_accept,"invalid CVTA address accepted");
        bool failed = false; try { reference(c); } catch (const std::exception&) { failed = true; }
        require(failed,"CVTA reference accepted invalid address"); b.tick();
        c.mask &= ~(1u<<7); execute(b,c); c.mask = UINT32_MAX;
    }
    const std::array<std::array<uint32_t,4>,6> faults = {{{99,0,0,1},{14,6,0,2},{3,0,7,3},{43,4,0,4},{7,0,0,6},{1,0,0,7}}};
    for (const auto& f : faults) {
        c.opcode = f[0]; c.mode = f[1]; c.immediate = f[2]; c.count = f[3] == 6 ? 0 : 3; c.latency = f[3] == 7 ? 0 : 1;
        drive(b,c); require(b.dut.input_error == f[3] && !b.dut.input_accept,"integer fault code differs"); b.tick();
        require(!b.dut.occupancy,"rejected command consumed result capacity");
    }
    c = Command{}; c.mask = 0; c.opcode = 14; c.mode = 255; execute(b,c);
    c.opcode = 3; c.immediate = UINT32_MAX; execute(b,c);
    c.opcode = 43; drive(b,c); require(b.dut.input_error == 4,"inactive CVTA mode validation lost");
}

void capacity_hold_reuse_and_reset() {
    TestBench b("integer-capacity.vcd"); Command c; c.opcode = 1; c.latency = 1;
    for (uint32_t entry = 0; entry < COLLECTOR_ENTRIES; ++entry) {
        c.entry = entry; c.immediate = 100+entry; drive(b,c); require(b.dut.input_accept,"integer queue filled early"); b.tick();
    }
    c.entry = 31; c.immediate = 999; drive(b,c);
    require(!b.dut.input_ready && !b.dut.input_accept && b.dut.occupancy == COLLECTOR_ENTRIES,"integer capacity absent");
    Vector expected; expected.fill(100);
    for (uint32_t wait = 0; wait < 9; ++wait) { check_output(b,0,expected); b.tick(); }
    b.dut.output_ready = 1; b.dut.eval(); require(b.dut.input_accept,"full pop/push did not return credit"); b.tick();
    b.dut.input_valid = 0; b.dut.eval();
    for (uint32_t entry = 1; entry < COLLECTOR_ENTRIES; ++entry) {
        expected.fill(100+entry); check_output(b,entry,expected); b.tick();
    }
    expected.fill(999); check_output(b,31,expected); b.tick();
    b.dut.output_ready = 0; c.latency = 100; drive(b,c); b.tick();
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.input_valid = 0; b.dut.eval();
    require(!b.dut.output_valid && !b.dut.occupancy,"reset did not discard in-flight integer result");
    for (uint32_t latency : {1,4,100}) { c.latency = latency; execute(b,c); }
}

struct Pending { uint64_t due; uint32_t entry; Vector value; };
void randomized_stream_with_independent_time_reference() {
    TestBench b(nullptr); std::mt19937 random(930714); std::deque<Pending> pending;
    for (uint32_t cycle = 0; cycle < 6000 || !pending.empty(); ++cycle) {
        Command c; c.opcode = random()%15; c.mode = random()%6; c.immediate = c.opcode == 3 ? random()%7 : random();
        c.mask = random(); c.entry = random()%32; c.latency = 1+random()%100;
        for (auto& source : c.data) for (auto& word : source) word = random();
        drive(b,c); b.dut.input_valid = cycle < 6000 && random()%4 != 0;
        b.dut.output_ready = random()%3 != 0; b.dut.eval();
        const bool ready = !pending.empty() && pending.front().due <= b.cycle;
        require(bool(b.dut.output_valid) == ready && b.dut.occupancy == pending.size(),"integer queue time/occupancy differs");
        if (ready) check_output(b,pending.front().entry,pending.front().value);
        const bool pop = ready && b.dut.output_ready, push = b.dut.input_valid && (pending.size() < COLLECTOR_ENTRIES || pop);
        require(bool(b.dut.input_accept) == push,"integer random backpressure differs");
        if (pop) pending.pop_front();
        if (push) pending.push_back({b.cycle+c.latency,c.entry,reference(c)});
        b.tick(); require(cycle < 6500,"integer random stream failed to drain");
    }
}

void run_cases() {
    run_case("integer all scalar opcodes, comparisons, special registers, masks and R63",arithmetic_modes_and_special_values);
    run_case("integer CVTA boundaries and rejected commands without partial capture",address_conversion_and_faults);
    run_case("integer finite capacity, stable output, reuse, reset and latency 1/4/100",capacity_hold_reuse_and_reset);
    run_case("integer randomized stream with independent latency and backpressure reference",randomized_stream_with_independent_time_reference);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

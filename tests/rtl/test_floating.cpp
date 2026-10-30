#include "bench.hpp"
#include "Vfloating_execute.h"
#include "simt/execution.hpp"
#include <cfenv>
#include <deque>
#include <random>

namespace {
using Vector = std::array<uint32_t,32>;
struct Command {
    uint32_t opcode = 18, mode = 0, mask = UINT32_MAX;
    uint32_t ra = 1, rb = 2, rc = 3, count = 3, entry = 0, latency = 4;
    std::array<Vector,3> data{};
};
class TestBench : public Bench<Vfloating_execute> {
public:
    explicit TestBench(const char* path) : Bench(path) {}
    void tick() { Bench::tick(); require(!dut.protocol_error,"floating lane/queue protocol differs"); }
};

void drive(TestBench& b, const Command& c) {
    auto& d = b.dut;
    d.input_valid = 1; d.opcode = c.opcode; d.mode = c.mode; d.mask = c.mask;
    d.ra = c.ra; d.rb = c.rb; d.rc = c.rc; d.source_count = c.count; d.source_registers = 1 | (2<<6) | (3<<12);
    d.input_entry = c.entry; d.latency = c.latency;
    for (uint32_t source = 0; source < 3; ++source)
        for (uint32_t lane = 0; lane < 32; ++lane) d.source_data[source*32+lane] = c.data[source][lane];
    d.eval();
}

// The pre-existing functional ISA, including checked conversion and NaN rules, is the numerical oracle.
// 既有功能 ISA 是数值参考，包含受检查的转换和 NaN 规则。
Vector reference(const Command& c) {
    simt::Instruction instruction;
    instruction.op = static_cast<simt::Op>(c.opcode); instruction.mode = c.mode;
    instruction.ra = c.ra; instruction.rb = c.rb; instruction.rc = c.rc; instruction.rd = 4;
    simt::Warp before, after; simt::Cta cta; simt::Config config; simt::Memory memory;
    for (uint32_t source = 0; source < c.count; ++source) before.r[source+1] = c.data[source];
    const simt::ExecutionContext context{instruction,cta,before,after,config,0,memory,memory};
    for (uint32_t lane = 0; lane < 32; ++lane)
        if (c.mask & (1u<<lane)) simt::execute_scalar_lane(context,lane);
    if (instruction.op != simt::Op::FSETP) return after.r[4];
    Vector result{}; result[0] = after.predicates[4]; return result;
}

void check_output(TestBench& b, uint32_t entry, const Vector& expected) {
    require(b.dut.output_valid && b.dut.output_entry == entry,"floating completion identity/valid differs");
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(b.dut.output_data[lane] == expected[lane],"floating result differs at lane "+std::to_string(lane));
}

void execute(TestBench& b, const Command& c) {
    const auto expected = reference(c); drive(b,c);
    require(b.dut.input_accept && !b.dut.input_error,"legal floating input rejected"); b.tick();
    b.dut.input_valid = 0; b.dut.eval();
    for (uint32_t step = 1; step < c.latency; ++step) {
        require(!b.dut.output_valid,"floating result bypassed a required stage"); b.tick();
    }
    check_output(b,c.entry,expected); b.dut.output_ready = 1; b.tick();
    b.dut.output_ready = 0; b.dut.eval(); require(!b.dut.occupancy,"floating result slot leaked");
}

void arithmetic_comparison_corners_and_masks() {
    TestBench b(nullptr); Command c;
    constexpr uint32_t points[] = {0,0x80000000,1,0x80000001,0x007fffff,0x00800000,0x3f800000,0xbf800000,
        0x3f800001,0x3f7ffffe,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,0x7f800001};
    for (uint32_t op : {15,16,17,18}) for (uint32_t i = 0; i < 256; ++i) {
        c.opcode = op; c.mode = i%4; c.mask = i%3 == 0 ? 0 : i%3 == 1 ? UINT32_MAX : 0xaaaaaaaa;
        for (uint32_t lane = 0; lane < 32; ++lane) {
            c.data[0][lane] = points[(i/16+lane)%16]; c.data[1][lane] = points[(i%16+lane)%16];
            c.data[2][lane] = points[(i+lane*7)%16];
        }
        execute(b,c);
    }
    c.opcode = 18; c.mask = UINT32_MAX; c.data[0].fill(0x3f800001); c.data[1].fill(0x3f7ffffe); c.data[2].fill(0xbf800000);
    require(reference(c)[0] == 0xa8800000,"fused-rounding reference not established"); execute(b,c);
    c.ra = c.rb = 1; c.rc = 63; c.count = 1; execute(b,c);
    c.ra = c.rb = 63; c.count = 0; execute(b,c);
}

void conversion_rounding_and_checked_boundaries() {
    TestBench b(nullptr); Command c; c.opcode = 22; c.count = 1; std::mt19937 random(61794);
    for (uint32_t sample = 0; sample < 4096; ++sample) {
        for (auto& value : c.data[0]) value = random();
        execute(b,c);
    }
    for (uint32_t power = 23; power < 32; ++power)
        for (int32_t offset = -257; offset <= 257; ++offset) {
            c.data[0].fill((1u<<power)+uint32_t(offset)); execute(b,c);
        }
    c.opcode = 23;
    for (uint32_t value : {0u,0x80000000u,1u,0x80000001u,0x3f7fffffu,0xbf7fffffu,0x3fc00000u,0xbfc00000u,
                          0x4effffffu,0xceffffffu,0xcf000000u}) {
        c.data[0].fill(value); execute(b,c);
    }
    for (uint32_t value : {0x4f000000u,0xcf000001u,0x7f800000u,0xff800000u,0x7fc00000u,0x7f800001u}) {
        c.data[0].fill(0); c.data[0][7] = value; drive(b,c);
        require(b.dut.input_error == 3 && !b.dut.input_accept,"invalid float-to-int committed a partial vector"); b.tick();
        require(!b.dut.occupancy,"invalid float-to-int reserved capacity");
        bool failed = false; try { reference(c); } catch (const std::exception&) { failed = true; }
        require(failed,"conversion boundary differs from ISA");
        c.mask &= ~(1u<<7); execute(b,c); c.mask = UINT32_MAX;
    }
}

void capacity_stability_reuse_and_reset() {
    TestBench b("floating-capacity.vcd"); Command c; c.opcode = 22; c.count = 1;
    for (uint32_t entry = 0; entry < COLLECTOR_ENTRIES; ++entry) {
        c.entry = entry; c.data[0].fill(100+entry); drive(b,c);
        require(b.dut.input_accept,"floating reservation queue filled early"); b.tick();
    }
    c.entry = 31; c.data[0].fill(999); drive(b,c);
    require(!b.dut.input_ready && !b.dut.input_accept,"floating capacity includes unreserved in-flight operations");
    for (uint32_t wait = 0; wait < 4; ++wait) b.tick();
    Command first; first.opcode = 22; first.data[0].fill(100);
    for (uint32_t wait = 0; wait < 9; ++wait) { check_output(b,0,reference(first)); b.tick(); }
    b.dut.output_ready = 1; b.dut.eval(); require(b.dut.input_accept,"floating full pop/push did not return credit"); b.tick();
    b.dut.input_valid = 0; b.dut.eval();
    for (uint32_t entry = 1; entry < COLLECTOR_ENTRIES; ++entry) {
        first.data[0].fill(100+entry); check_output(b,entry,reference(first)); b.tick();
    }
    while (!b.dut.output_valid) b.tick();
    check_output(b,31,reference(c)); b.tick();
    b.dut.output_ready = 0; c.latency = 100; drive(b,c); b.tick();
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.input_valid = 0; b.dut.eval();
    for (uint32_t wait = 0; wait < 5; ++wait) { b.tick(); require(!b.dut.output_valid && !b.dut.occupancy,"reset retained floating work"); }
    for (uint32_t latency : {4,6,100}) { c.latency = latency; execute(b,c); }
}

void validation_without_capture() {
    TestBench b("floating-errors.vcd"); Command c;
    const std::array<std::array<uint32_t,3>,4> faults = {{{99,0,1},{15,4,2},{18,0,4},{16,0,5}}};
    for (const auto& fault : faults) {
        c.opcode = fault[0]; c.mode = fault[1]; c.count = fault[2] == 4 ? 0 : 3; c.latency = fault[2] == 5 ? 3 : 4;
        drive(b,c); require(b.dut.input_error == fault[2] && !b.dut.input_accept,"floating validation differs"); b.tick();
        require(!b.dut.occupancy,"rejected command consumed floating capacity");
    }
    c.opcode = 15; c.mode = 255; c.mask = 0; c.count = 0; c.latency = 4; execute(b,c);
}

Command random_command(std::mt19937& random) {
    constexpr uint32_t ops[] = {15,16,17,18,22,23}; Command c;
    c.opcode = ops[random()%6]; c.mode = random()%4; c.mask = random(); c.entry = random()%32; c.latency = 4+random()%97;
    for (auto& source : c.data) for (auto& value : source) value = random();
    if (c.opcode == 23) for (auto& value : c.data[0]) value = (value & 0x807fffff) | ((random()%158)<<23);
    return c;
}
struct Pending { uint64_t due; uint32_t entry; Vector value; };
void mixed_stream_with_independent_clock_reference() {
    TestBench b(nullptr); std::mt19937 random(901748); std::deque<Pending> pending;
    for (uint32_t cycle = 0; cycle < 6000 || !pending.empty(); ++cycle) {
        const auto c = random_command(random); drive(b,c);
        b.dut.input_valid = cycle < 6000 && random()%4 != 0; b.dut.output_ready = random()%3 != 0; b.dut.eval();
        const bool ready = !pending.empty() && pending.front().due <= b.cycle;
        require(bool(b.dut.output_valid) == ready && b.dut.occupancy == pending.size(),"floating physical clock/capacity differs");
        if (ready) check_output(b,pending.front().entry,pending.front().value);
        const bool pop = ready && b.dut.output_ready, push = b.dut.input_valid && (pending.size() < COLLECTOR_ENTRIES || pop);
        require(bool(b.dut.input_accept) == push,"floating stream backpressure differs");
        if (pop) pending.pop_front();
        if (push) pending.push_back({b.cycle+c.latency,c.entry,reference(c)});
        b.tick(); require(cycle < 6500,"floating stream failed to drain");
    }
}

void run_cases() {
    require(std::fesetround(FE_TONEAREST) == 0,"cannot establish rounding reference");
    run_case("floating arithmetic, unordered comparisons, signed zeros, masks and fused rounding",arithmetic_comparison_corners_and_masks);
    run_case("floating conversion random integers, rounding boundaries and checked truncation",conversion_rounding_and_checked_boundaries);
    run_case("floating physical stages, finite reservations, stable results, reuse and reset",capacity_stability_reuse_and_reset);
    run_case("floating invalid commands reject before partial capture",validation_without_capture);
    run_case("floating mixed stream with independent clocks and backpressure reference",mixed_stream_with_independent_clock_reference);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

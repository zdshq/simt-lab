#include "bench.hpp"
#include "Vpredicate_file.h"
#include <array>
#include <random>

namespace {
using TestBench = Bench<Vpredicate_file>;
constexpr uint32_t slots = SIMT_RTL_SLOTS;
using Reference = std::array<std::array<uint32_t,7>,slots>;

void commit(TestBench& b) {
    b.tick(); b.dut.write_valid = 0; b.dut.clear_slots = 0; b.dut.eval();
}
void write(TestBench& b, uint32_t slot, uint32_t reg, uint32_t mask, uint32_t value) {
    b.dut.write_slot = slot; b.dut.write_register = reg;
    b.dut.write_mask = mask; b.dut.write_data = value; b.dut.write_valid = 1; b.dut.eval();
}
void query(TestBench& b, uint32_t slot, uint32_t guard, uint32_t source, uint32_t group) {
    b.dut.query_slot = slot; b.dut.query_guard = guard;
    b.dut.query_source = source; b.dut.query_group = group; b.dut.eval();
}

void constants_masks_and_edges() {
    TestBench b("predicate-ports.vcd");
    for (uint32_t slot = 0; slot < slots; ++slot) {
        for (uint32_t reg = 0; reg < 7; ++reg) {
            query(b,slot,reg,7,UINT32_MAX);
            require(b.dut.participating == 0 && b.dut.source_data == UINT32_MAX,"reset or P7 differs");
            query(b,slot,reg+8,reg,0x80000103);
            require(b.dut.participating == 0x80000103 && b.dut.source_data == 0,"inversion escaped group");
        }
    }
    const auto cycle = b.cycle;
    write(b,slots-1,6,UINT32_MAX,0xa5a5a5a5);
    query(b,slots-1,6,6,UINT32_MAX);
    require(b.dut.participating == 0xa5a5a5a5 && b.dut.source_data == 0xa5a5a5a5,"two-port bypass differs");
    b.dut.write_valid = 0; b.dut.eval();
    require(b.dut.participating == 0 && b.cycle == cycle,"query or unaccepted write changed storage");
    write(b,slots-1,6,UINT32_MAX,0xa5a5a5a5); commit(b);
    require(b.dut.participating == 0xa5a5a5a5 && !b.dut.clock_enable,"edge did not retain data or idle activity");
}

void masked_clear_and_isolation() {
    TestBench b("predicate-reuse.vcd");
    write(b,0,0,UINT32_MAX,UINT32_MAX); commit(b);
    write(b,slots-1,0,UINT32_MAX,0x12345678); commit(b);
    write(b,0,0,0x80000001,0); query(b,0,0,0,UINT32_MAX);
    require(b.dut.participating == 0x7ffffffe,"masked write destroyed unselected lanes"); commit(b);
    b.dut.clear_slots = 1; b.dut.eval();
    require(b.dut.participating == 0,"clear failed before edge");
    write(b,0,0,2,2);
    require(b.dut.participating == 2,"clear/write order resurrected old bits"); commit(b);
    require(b.dut.participating == 2,"clear/write order changed after edge");
    query(b,slots-1,0,0,UINT32_MAX);
    require(b.dut.participating == 0x12345678,"slot clear damaged another warp");
    b.dut.reset = 1; b.dut.eval(); b.tick(); b.dut.reset = 0; b.dut.eval();
    require(b.dut.participating == 0,"reset retained predicate ownership");
}

void read_only_constant_and_empty_write() {
    TestBench b(nullptr);
    query(b,0,7,7,0x80000001);
    write(b,0,7,UINT32_MAX,0);
    require(b.dut.write_error && !b.dut.write_accept,"P7 write was accepted"); commit(b);
    require(b.dut.participating == 0x80000001 && b.dut.source_data == UINT32_MAX,"P7 mutated");
    query(b,0,15,0,UINT32_MAX);
    require(!b.dut.participating,"inverted P7 is not false");
    write(b,0,0,0,UINT32_MAX);
    require(!b.dut.write_accept && !b.dut.write_error && !b.dut.clock_enable,"empty write changed activity"); commit(b);
    require(!b.dut.source_data,"empty write changed data");
}

// Reference words update only at accepted writes; compare all three combinational ports before and after capture.
// 参考字仅在已接受写入时更新；在捕获前后分别核对三个组合读取端口。
void check_query(TestBench& b, const Reference& values, std::mt19937& random) {
    const uint32_t slot = random()%slots, guard = random()%16, source = random()%8, group = random();
    query(b,slot,guard,source,group);
    uint32_t active = (guard&7) == 7 ? UINT32_MAX : values[slot][guard&7];
    if (guard&8) active = ~active;
    require(b.dut.participating == (active&group),"random guard/group differs");
    require(b.dut.source_data == (source == 7 ? UINT32_MAX : values[slot][source]),"random source differs");
    const uint32_t execution_slot = random()%slots, execution_source = random()%8;
    b.dut.execution_slot = execution_slot; b.dut.execution_source = execution_source; b.dut.eval();
    require(b.dut.execution_data == (execution_source == 7 ? UINT32_MAX : values[execution_slot][execution_source]),
            "independent execution predicate port differs");
}
void randomized_reference() {
    TestBench b(nullptr); Reference values{}; std::mt19937 random(20261031);
    for (uint32_t cycle = 0; cycle < 12000; ++cycle) {
        if (cycle%13 == 0) {
            const auto slot = random()%slots; b.dut.clear_slots = uint32_t{1}<<slot; values[slot] = {};
        }
        const uint32_t slot = random()%slots, reg = random()%8, mask = random(), data = random();
        write(b,slot,reg,mask,data);
        if (reg < 7) values[slot][reg] = (values[slot][reg]&~mask)|(data&mask);
        check_query(b,values,random); commit(b); check_query(b,values,random);
    }
}

void run_cases() {
    run_case("predicate constants dual ports masks and edge ownership",constants_masks_and_edges);
    run_case("predicate masked writes clear ordering and warp isolation",masked_clear_and_isolation);
    run_case("predicate P7 rejection and zero-mask write",read_only_constant_and_empty_write);
    run_case("predicate 12000-cycle independent randomized reference",randomized_reference);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

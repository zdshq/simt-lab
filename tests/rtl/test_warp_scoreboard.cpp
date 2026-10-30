#include "bench.hpp"
#include "Vwarp_scoreboard.h"
#include "simt/hardware_pipeline.hpp"
#include <random>

namespace {
using Reference = simt::hw::Scoreboard;
using TestBench = Bench<Vwarp_scoreboard>;

void apply(Reference& reference, uint32_t operation, uint32_t reg, uint32_t mask) {
    switch (operation) {
    case 0: reference.reserve_read(reg, mask); break;
    case 1: reference.collect_read(reg, mask); break;
    case 2: reference.reserve_write(reg, mask); break;
    case 3: reference.retire_write(reg, mask); break;
    case 4: reference.reserve_predicate(reg, mask); break;
    case 5: reference.retire_predicate(reg, mask); break;
    default: throw std::runtime_error("invalid command");
    }
}

// Pack individual bits independently of the production adapter's word-shift implementation.
// 逐位打包，与生产适配器的字移位实现保持独立。
void pins(Vwarp_scoreboard& dut, uint32_t port, uint32_t op, uint32_t reg, uint32_t mask) {
    dut.command = (dut.command & ~(uint64_t{7} << (port*3))) | (uint64_t{op} << (port*3));
    for (uint32_t bit = 0; bit < 6; ++bit) {
        const auto target = port*6+bit, flag = uint32_t{1} << (target%32);
        auto& word = dut.command_register[target/32];
        word = (word & ~flag) | ((reg & (1u << bit)) ? flag : 0);
    }
    dut.command_mask[port] = mask; dut.command_valid |= 1u << port; dut.eval();
}

void compare(Vwarp_scoreboard& dut, const Reference& reference, uint32_t reg, uint32_t mask) {
    dut.query_register = reg; dut.query_predicate = reg%8; dut.query_mask = mask; dut.eval();
    require(bool(dut.readable) == reference.readable(reg, mask), "forwarded RAW differs");
    require(bool(dut.writable) == reference.writable(reg, mask), "forwarded WAR/WAW differs");
    require(bool(dut.predicate_ready) == reference.predicate_ready(reg%8, mask), "forwarded predicate differs");
}

bool event(TestBench& b, Reference& reference, uint32_t port, uint32_t op, uint32_t reg, uint32_t mask) {
    auto next = reference;
    bool valid = true;
    try { apply(next, op, reg, mask); } catch (const std::exception&) { valid = false; }
    pins(b.dut, port, op, reg, mask);
    require(bool(b.dut.command_error & (1u << port)) == !valid, "event legality differs");
    const bool accepted = b.dut.command_ready & (1u << port);
    if (accepted) reference = next;
    else { b.dut.command_valid &= ~(1u << port); b.dut.eval(); }
    compare(b.dut, reference, reg, mask);
    return accepted;
}

void commit(TestBench& b) {
    b.tick(); b.dut.command_valid = 0; b.dut.eval();
}

void combinational_forwarding_and_clock() {
    TestBench b("warp-scoreboard-clock.vcd"); Reference reference;
    require(!b.dut.clock_enable, "idle event bus enables storage clocks");
    event(b, reference, 0, 2, 3, 7);
    require(b.dut.clock_enable, "reservation did not enable storage clocks");
    require(!b.dut.readable, "same-cycle reservation absent");
    b.dut.command_valid = 0; b.dut.eval();
    require(!b.dut.clock_enable, "withdrawn event left clock gate open");
    require(b.dut.readable, "combinational evaluation committed state");
    b.dut.command_valid = 1; commit(b);
    compare(b.dut, reference, 3, 7);
    event(b, reference, 0, 3, 3, 7);
    event(b, reference, 1, 0, 3, 7);
    event(b, reference, 2, 2, 3, 7);
    commit(b);
    event(b, reference, 0, 3, 3, 7);
    event(b, reference, 1, 1, 3, 7);
    commit(b); compare(b.dut, reference, 3, 7);
}

void masks_predicates_and_atomic_rejection() {
    TestBench b("warp-scoreboard-masks.vcd"); Reference reference;
    event(b, reference, 0, 0, 9, 1); commit(b);
    require(!event(b, reference, 1, 1, 9, 3), "partial invalid collection accepted");
    event(b, reference, 2, 1, 9, 1);
    event(b, reference, 3, 2, 9, 2);
    commit(b);
    event(b, reference, 4, 2, 9, 1);
    require(!event(b, reference, 5, 2, 9, 3), "overlapping writers accepted");
    event(b, reference, 6, 4, 2, 15);
    require(!event(b, reference, 7, 5, 2, 31), "partial invalid predicate retirement accepted");
    event(b, reference, 8, 5, 2, 15);
    event(b, reference, 9, 4, 2, 7);
    for (uint32_t op = 0; op < 4; ++op) event(b, reference, 10+op, op, 63, UINT32_MAX);
    commit(b);
    for (uint32_t reg = 0; reg < 64; ++reg) compare(b.dut, reference, reg, UINT32_MAX);
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval();
    Reference empty;
    for (uint32_t reg = 0; reg < 64; ++reg) compare(b.dut, empty, reg, UINT32_MAX);
}

void reader_saturation_and_release_forwarding() {
    TestBench b("warp-scoreboard-capacity.vcd"); Reference reference;
    for (uint32_t index = 0; index < 63; ++index) {
        event(b, reference, index%16, 0, 4, 1);
        commit(b);
    }
    commit(b);
    require(!event(b, reference, 0, 0, 4, 1), "reader overflow accepted");
    require(!(b.dut.command_error & 1), "saturation must be backpressure");
    event(b, reference, 1, 1, 4, 1);
    require(event(b, reference, 2, 0, 4, 1), "same-cycle release did not free reader credit");
    commit(b);
    for (uint32_t index = 0; index < 63; ++index) {
        event(b, reference, index%16, 1, 4, 1);
        commit(b);
    }
    commit(b); require(b.dut.writable, "reader counter did not return to zero");
}

void randomized_event_stream() {
    TestBench b(nullptr); Reference reference; std::mt19937 random(41023);
    for (uint32_t cycle = 0; cycle < 400; ++cycle) {
        const uint32_t phases[16] = {3,3,3,3,5,1,1,1,1,0,0,0,2,2,4,4};
        uint64_t used[6]{};
        for (uint32_t port = 0; port < 16; ++port) {
            const auto op = phases[port];
            uint32_t reg = 0;
            do { reg = op >= 4 ? random()%8 : random()%64; } while (used[op] & (uint64_t{1} << reg));
            used[op] |= uint64_t{1} << reg;
            const auto mask = uint32_t{1} << (random()%32);
            event(b, reference, port, op, reg, mask);
            compare(b.dut, reference, random()%64, random());
        }
        commit(b);
        for (uint32_t reg = 0; reg < 64; ++reg) compare(b.dut, reference, reg, UINT32_MAX);
    }
}

// Instruction ports observe the same forwarding state while legacy query inputs change independently.
// 指令端口读取同一份旁路状态，旧查询输入可独立变化。
void parallel_issue_ports_and_forwarding() {
    TestBench b(nullptr); Reference reference;
    event(b,reference,0,2,5,1); event(b,reference,1,2,62,0x80000000);
    event(b,reference,2,0,9,3); event(b,reference,3,4,6,32); commit(b);
    b.dut.issue_source_registers=5|(62<<6)|(63<<12);
    b.dut.issue_source_masks[0]=1; b.dut.issue_source_masks[1]=0x80000000; b.dut.issue_source_masks[2]=UINT32_MAX;
    b.dut.issue_destination=9; b.dut.issue_write_mask=2;
    b.dut.issue_predicate=6; b.dut.issue_predicate_mask=32;
    b.dut.issue_source_predicate=6; b.dut.issue_source_predicate_mask=64;
    for (uint32_t reg=0; reg<64; ++reg) {
        compare(b.dut,reference,reg,UINT32_MAX);
        require(b.dut.issue_blocked==51 && !b.dut.issue_ready,"parallel query depends on legacy query pins");
    }
    event(b,reference,0,3,5,1); event(b,reference,1,3,62,0x80000000);
    event(b,reference,2,1,9,2); event(b,reference,3,5,6,32);
    require(b.dut.issue_ready && !b.dut.issue_blocked,"retire/collect bypass did not reach all issue ports");
    b.dut.command_valid=0; b.dut.eval();
    require(b.dut.issue_blocked==51,"query committed pending releases without a clock");
    b.dut.command_valid=15; commit(b);
    require(b.dut.issue_ready,"committed releases were lost");
    event(b,reference,0,0,5,1); event(b,reference,1,2,5,1);
    b.dut.issue_destination=5; b.dut.issue_write_mask=1; b.dut.eval();
    require(b.dut.issue_blocked==25,"read-modify-write forwarding lost RAW/WAW/WAR distinction");
}

void run_cases() {
    run_case("warp scoreboard forwarding and clock ownership", combinational_forwarding_and_clock);
    run_case("warp scoreboard masks predicates atomic rejection and reset", masks_predicates_and_atomic_rejection);
    run_case("warp scoreboard bounded readers and same-cycle release", reader_saturation_and_release_forwarding);
    run_case("warp scoreboard 6400-event independent differential", randomized_event_stream);
    run_case("warp scoreboard parallel instruction ports and same-cycle forwarding",parallel_issue_ports_and_forwarding);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

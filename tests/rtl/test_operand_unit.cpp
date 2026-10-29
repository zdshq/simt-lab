#include "bench.hpp"
#include "Vregister_operand_unit.h"
#include <bit>

namespace {
class TestBench : public Bench<Vregister_operand_unit> {
public:
    explicit TestBench(const char* name) : Bench(name) {
        dut.port_groups = 4; dut.entry_limit = 4; dut.eval();
    }
};

void commit(TestBench& b) {
    b.tick(); b.dut.write_valid = 0; b.dut.clear_slots = 0;
    b.dut.allocate_valid = 0; b.dut.release_entries = 0; b.dut.collect_enable = 0; b.dut.eval();
}

void write(TestBench& b, uint32_t slot, uint32_t reg, uint32_t value) {
    const uint32_t bank = reg%4, width = std::countr_zero(uint32_t{b.dut.slot_count})+4;
    const uint64_t mask = (uint64_t{1}<<width)-1, shift = bank*width;
    b.dut.write_rows = (b.dut.write_rows & ~(mask<<shift)) | (uint64_t{slot*16+reg/4}<<shift);
    b.dut.write_masks[bank] = UINT32_MAX;
    for (uint32_t lane = 0; lane < 32; ++lane) b.dut.write_data[bank*32+lane] = value+lane;
    b.dut.write_valid |= 1u<<bank; b.dut.eval();
}

uint32_t allocate(TestBench& b, uint32_t slot, uint32_t first, uint32_t second) {
    b.dut.allocate_warp = slot; b.dut.allocate_count = 2;
    b.dut.allocate_registers = first | (second<<6);
    b.dut.allocate_masks[0] = b.dut.allocate_masks[1] = UINT32_MAX;
    b.dut.allocate_valid = 1; b.dut.eval();
    require(b.dut.allocate_ready, "connected allocation rejected"); return b.dut.allocate_index;
}

void direct_storage_and_collector_retention() {
    TestBench b("operand-unit-retention.vcd"); const auto slot = b.dut.slot_count-1;
    write(b,slot,0,11); commit(b); write(b,slot,4,101); commit(b);
    const auto id = allocate(b,slot,0,4); commit(b);
    b.dut.collect_enable = 1; b.dut.eval();
    require(b.dut.read_events == 1 && !b.dut.ready_entries, "connected bank conflict not serialized"); commit(b);
    write(b,slot,0,999); b.dut.collect_enable = 1; b.dut.query_entry = id; b.dut.eval();
    require(b.dut.ready_entries & (1u<<id), "connected collector failed to finish");
    for (uint32_t lane = 0; lane < 32; ++lane) {
        require(b.dut.query_data[lane] == 11+lane, "collector used overwritten RF data");
        require(b.dut.query_data[32+lane] == 101+lane, "RF to collector wire data differs");
    }
    commit(b); b.dut.clear_slots = uint32_t{1}<<slot; b.dut.eval(); commit(b);
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(b.dut.query_data[lane] == 11+lane, "RF slot clear erased held collector value");
}

void same_cycle_writeback_forwarding_and_slot_isolation() {
    TestBench b("operand-unit-bypass.vcd");
    write(b,1,2,700); commit(b);
    const auto id = allocate(b,0,1,2); commit(b);
    write(b,0,1,42); b.dut.collect_enable = 1; b.dut.query_entry = id; b.dut.eval();
    require(b.dut.read_events == 6 && (b.dut.ready_entries & (1u<<id)), "independent banks not collected together");
    for (uint32_t lane = 0; lane < 32; ++lane) {
        require(b.dut.query_data[lane] == 42+lane, "same-cycle writeback bypass absent");
        require(b.dut.query_data[32+lane] == 0, "another warp's register leaked");
    }
    b.dut.release_entries = 1u<<id; b.dut.eval();
    require(allocate(b,1,1,2) == id, "connected same-cycle release/reallocation failed"); commit(b);
    b.dut.collect_enable = 1; b.dut.eval();
    for (uint32_t lane = 0; lane < 32; ++lane) {
        require(b.dut.query_data[lane] == 0, "reused collector inherited old source");
        require(b.dut.query_data[32+lane] == 700+lane, "reused collector selected wrong warp");
    }
}

void run_cases() {
    run_case("connected RF collector old values and slot-clear isolation",direct_storage_and_collector_retention);
    run_case("connected RF collector writeback bypass and same-cycle reuse",same_cycle_writeback_forwarding_and_slot_isolation);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

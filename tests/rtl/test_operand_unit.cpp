#include "bench.hpp"
#include "Vregister_operand_unit.h"
#include <bit>

namespace {
class TestBench : public Bench<Vregister_operand_unit> {
public:
    explicit TestBench(const char* name) : Bench(name) {
        dut.allocate_pipe = 0; dut.dispatch_enable = 0; dut.dispatch_accept = 0;
        dut.port_groups = 4; dut.entry_limit = 4; dut.eval();
    }
};

void commit(TestBench& b) {
    b.tick(); b.dut.write_valid = 0; b.dut.clear_slots = 0;
    b.dut.integer_launch = 0;
    b.dut.floating_launch = 0;
    b.dut.wb_reserve_valid = 0; b.dut.wb_complete_valid = 0; b.dut.wb_acknowledge = 0;
    b.dut.allocate_valid = 0; b.dut.release_entries = 0; b.dut.collect_enable = 0; b.dut.dispatch_enable = 0; b.dut.dispatch_accept = 0; b.dut.eval();
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

// The highest execution port receives RF bypass data and reuses the entry for a different pipe.
// 最高编号执行端口接收寄存器旁路数据，并将条目复用到另一流水线。
void physical_execution_bus_bypass_and_reuse() {
    TestBench b("operand-unit-dispatch.vcd");
    write(b,0,1,700); commit(b); b.dut.allocate_pipe = 6;
    const auto id = allocate(b,0,1,2); commit(b);
    write(b,0,2,42); b.dut.collect_enable = 1;
    b.dut.dispatch_enable = 1; b.dut.dispatch_eligible = UINT32_MAX; b.dut.eval();
    require(b.dut.dispatch_valid == 64 && ((b.dut.dispatch_entries>>30)&31) == id,
            "highest physical execution port lost entry identity");
    require(((b.dut.dispatch_counts>>12)&3) == 2, "execution source count differs");
    for (uint32_t lane = 0; lane < 32; ++lane) {
        require(b.dut.dispatch_data[6*96+lane] == 700+lane, "execution bus did not carry retained RF value");
        require(b.dut.dispatch_data[6*96+32+lane] == 42+lane, "execution bus lost same-cycle writeback bypass");
    }
    b.dut.dispatch_accept = 64; b.dut.eval(); require(!b.dut.dispatch_error, "valid execution handshake rejected");
    b.dut.allocate_pipe = 1; require(allocate(b,1,1,2) == id, "execution acceptance failed to return slot"); commit(b);
    b.dut.collect_enable = 1; b.dut.dispatch_enable = 1; b.dut.eval();
    require(b.dut.dispatch_valid == 2, "reused slot retained previous execution pipe");
    for (uint32_t word = 0; word < 96; ++word)
        require(b.dut.dispatch_data[96+word] == 0, "reused execution bus leaked another warp's data");
}

// A retirement grant must both forward now and persist across the same source-clock edge.
// 退休授权必须既能立即旁路，又能跨越同一源时钟边沿持久保存。
void writeback_to_rf_and_collector() {
    TestBench b("operand-unit-writeback.vcd"); auto& d = b.dut;
    d.wb_reserve_warp = 1; d.wb_reserve_pipe = 6; d.wb_reserve_kind = 1;
    d.wb_reserve_register = 2; d.wb_reserve_mask = 0x80000001; d.wb_reserve_valid = 1; d.eval();
    require(d.wb_reserve_ready,"connected completion reservation refused");
    const auto completion = d.wb_reserve_index;
    const auto reader = allocate(b,1,2,3); commit(b);
    d.wb_complete_entries = uint64_t{completion}<<30;
    for (uint32_t lane = 0; lane < 32; ++lane) d.wb_complete_data[6*32+lane] = 42+lane;
    d.wb_complete_valid = 64; d.eval();
    require(d.wb_complete_accept == 64 && d.wb_retire_ready == (1u<<completion),"connected completion lost");
    d.wb_acknowledge = 1u<<completion; d.collect_enable = 1; d.query_entry = reader; d.eval();
    require(d.write_events == 4 && d.query_data[0] == 42 && d.query_data[31] == 73,
            "physical writeback did not forward to collector");
    require(d.query_data[1] == 0,"physical writeback ignored lane mask");
    d.release_entries = 1u<<reader; d.eval(); commit(b);
    require(d.wb_occupancy == 0 && !d.write_events,"retired result retained a write port");
    const auto later = allocate(b,1,2,3); commit(b);
    d.collect_enable = 1; d.query_entry = later; d.eval();
    require(d.query_data[0] == 42 && d.query_data[31] == 73 && d.query_data[1] == 0,
            "writeback forwarded but did not persist across the RF clock edge");
}

// A last-source read feeds the integer ALU in the same cycle; only circuit storage supplies the later read.
// 最后一个源读取在同拍送入整数 ALU；稍后的读取只由电路存储提供数据。
void integer_execution_connected_storage() {
    TestBench b("operand-unit-integer.vcd"); auto& d = b.dut;
    write(b,0,1,11); commit(b); write(b,0,2,101); commit(b);
    d.wb_reserve_warp = 0; d.wb_reserve_pipe = 0; d.wb_reserve_kind = 1;
    d.wb_reserve_register = 3; d.wb_reserve_mask = UINT32_MAX; d.wb_reserve_valid = 1; d.eval();
    const uint32_t result = d.wb_reserve_index, source = allocate(b,0,1,2); commit(b);
    d.collect_enable = 1; d.dispatch_enable = 1; d.dispatch_eligible = UINT32_MAX;
    d.integer_launch = 1; d.integer_collector = source; d.integer_entry = result;
    d.integer_opcode = 4; d.integer_ra = 1; d.integer_rb = 2; d.integer_rc = 63;
    d.integer_mask = UINT32_MAX; d.integer_latency = 1; d.eval();
    require(d.integer_accept && d.dispatch_accept_events == 1,"connected integer failed last-source acceptance");
    commit(b);
    require(d.occupancy == 0 && d.wb_completed_entries == (1u<<result),"integer collector release or WB completion missing");
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(d.wb_retire_data[3*32+lane] == 112+2*lane,"integer physical retirement bus differs");
    d.wb_acknowledge = 1u<<result; d.eval(); commit(b);
    const auto reader = allocate(b,0,3,4); commit(b);
    d.collect_enable = 1; d.query_entry = reader; d.eval();
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(d.query_data[lane] == 112+2*lane,"integer physical result did not persist in RF");
}

// A floating result must traverse four arithmetic registers and survive the configured extra delay before RF writeback.
// 浮点结果必须经过四级算术寄存器，并在配置的额外延迟后仍能写回 RF。
void floating_execution_connected_storage() {
    TestBench b("operand-unit-floating.vcd"); auto& d = b.dut;
    write(b,0,1,0x3f800000); commit(b); write(b,0,2,0x3f800000); commit(b);
    d.wb_reserve_warp = 0; d.wb_reserve_pipe = 1; d.wb_reserve_kind = 1;
    d.wb_reserve_register = 3; d.wb_reserve_mask = UINT32_MAX; d.wb_reserve_valid = 1; d.eval();
    d.allocate_pipe = 1;
    const uint32_t result = d.wb_reserve_index, source = allocate(b,0,1,2); commit(b);
    d.collect_enable = 1; d.dispatch_enable = 1; d.dispatch_eligible = UINT32_MAX;
    d.floating_launch = 1; d.floating_collector = source; d.floating_entry = result;
    d.floating_opcode = 16; d.floating_ra = 1; d.floating_rb = 2; d.floating_rc = 63;
    d.floating_mask = UINT32_MAX; d.floating_latency = 6; d.eval();
    require(d.floating_accept && d.dispatch_accept_events == 2,"connected floating failed last-source acceptance");
    for (uint32_t tick = 0; tick < 6; ++tick) {
        require(!d.wb_completed_entries,"floating completion bypassed configured stages"); commit(b);
    }
    require(d.occupancy == 0 && d.wb_completed_entries == (1u<<result),"floating release or completion missing");
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(d.wb_retire_data[3*32+lane] == 0x40000000+lane,"floating physical retirement data differs");
    d.wb_acknowledge = 1u<<result; d.eval(); commit(b);
    const auto reader = allocate(b,0,3,4); commit(b); d.collect_enable = 1; d.query_entry = reader; d.eval();
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(d.query_data[lane] == 0x40000000+lane,"floating result did not persist in RF");
}

// Predicate retirement drives the independent physical storage port and both issue read muxes.
// 谓词退休驱动独立物理存储端口及两个发射读取多路器。
void predicate_retirement_connection() {
    TestBench b("operand-unit-predicate.vcd"); const auto slot = b.dut.slot_count-1;
    b.dut.wb_reserve_valid = 1; b.dut.wb_reserve_warp = slot; b.dut.wb_reserve_pipe = 4;
    b.dut.wb_reserve_kind = 2; b.dut.wb_reserve_register = 6; b.dut.wb_reserve_mask = 0x80000001;
    b.dut.eval(); const auto id = b.dut.wb_reserve_index; commit(b);
    b.dut.predicate_query_slot = slot; b.dut.predicate_query_guard = 6;
    b.dut.predicate_query_source = 6; b.dut.predicate_query_group = UINT32_MAX;
    b.dut.wb_complete_valid = 16; b.dut.wb_complete_entries = uint64_t{id}<<20;
    b.dut.wb_complete_data[128] = UINT32_MAX; b.dut.eval();
    require(!b.dut.predicate_write_event && !b.dut.predicate_participating,"unfinished predicate reached storage");
    b.dut.wb_acknowledge = 1u<<id; b.dut.eval();
    require(b.dut.predicate_write_event && b.dut.predicate_port_busy,"acknowledgement missed physical predicate port");
    require(b.dut.predicate_participating == 0x80000001 && b.dut.predicate_source_data == 0x80000001,"retirement did not bypass to read muxes");
    commit(b);
    require(b.dut.predicate_participating == 0x80000001 && !b.dut.predicate_write_event,"predicate retirement did not persist");
    b.dut.predicate_query_slot = 0; b.dut.eval();
    require(!b.dut.predicate_participating,"predicate warp address truncated");
}

void run_cases() {
    run_case("connected predicate retirement mask bypass and persistent storage",predicate_retirement_connection);
    run_case("connected floating four arithmetic stages, result delay and physical RF storage",floating_execution_connected_storage);
    run_case("connected integer collector last-source bypass and physical result storage",integer_execution_connected_storage);
    run_case("connected writeback physical RF storage and collector bypass",writeback_to_rf_and_collector);
    run_case("connected RF physical dispatch bypass and pipe reuse",physical_execution_bus_bypass_and_reuse);
    run_case("connected RF collector old values and slot-clear isolation",direct_storage_and_collector_retention);
    run_case("connected RF collector writeback bypass and same-cycle reuse",same_cycle_writeback_forwarding_and_slot_isolation);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

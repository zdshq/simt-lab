#include "bench.hpp"
#include "Vwarp_control.h"
#include "simt/control.hpp"
#include <random>

namespace {
using TestBench = Bench<Vwarp_control>;
struct Command {
    uint32_t op = 26, token = 0, guard = 7, pc = 0, group = UINT32_MAX, mask = UINT32_MAX, target = 64;
    uint32_t code_size = 128;
    bool valid_target = true;
};
void drive(TestBench& b, const simt::Warp& w, const Command& c) {
    auto& d = b.dut; d.command_valid = 1; d.opcode = c.op; d.token = c.token; d.guard = c.guard;
    d.group_pc = c.pc; d.group_mask = c.group; d.mask = c.mask; d.target_pc = c.target;
    d.code_size = c.code_size; d.target_is_sync = c.valid_target;
    for (uint32_t word = 0; word < 3; ++word) d.lane_state[word] = 0;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        d.lane_pc[lane] = w.pc[lane]; d.lane_blocked[lane] = w.blocked_token[lane];
        const auto shift = lane*3, value = static_cast<uint32_t>(w.state[lane]);
        d.lane_state[shift/32] |= value<<(shift%32);
        if (shift%32 > 29) d.lane_state[shift/32+1] |= value>>(32-shift%32);
    }
    d.eval();
}
simt::Warp reference(const simt::Warp& before, const Command& c) {
    simt::Warp after = before;
    for (uint32_t lane = 0; lane < 32; ++lane) if (c.group & (1u<<lane)) after.pc[lane] = c.pc+1;
    simt::Instruction i; i.op = static_cast<simt::Op>(c.op); i.rd = c.token; i.guard = c.guard; i.imm = c.target;
    simt::Module module; module.code.resize(c.code_size);
    if (c.target < c.code_size && c.valid_target) { module.code[c.target].op = simt::Op::BSYNC; module.code[c.target].rd = c.token; }
    simt::Cta cta; simt::Config config; simt::Memory memory;
    const simt::ExecutionContext context{i,cta,before,after,config,0,memory,memory};
    if (!simt::execute_control(context,{c.pc,c.group},c.mask,module)) throw std::runtime_error("unsupported_control_opcode");
    return after;
}
void check_tokens(TestBench& b, const simt::Warp& w) {
    for (uint32_t id = 0; id < 16; ++id) {
        const auto& t = w.reconvergence[id];
        require(((b.dut.token_states>>(id*2))&3) == static_cast<uint32_t>(t.state),"token state mismatch");
        require(b.dut.token_members[id] == t.participants && b.dut.token_arrivals[id] == t.arrived &&
                b.dut.token_targets[id] == t.target_pc,"token payload mismatch");
    }
}
void check_lanes(TestBench& b, const simt::Warp& w) {
    for (uint32_t lane = 0; lane < 32; ++lane) {
        const auto shift = lane*3; uint64_t state = b.dut.next_state[shift/32];
        if (shift%32 > 29) state |= uint64_t{b.dut.next_state[shift/32+1]}<<32;
        require(b.dut.next_pc[lane] == w.pc[lane] && b.dut.next_blocked[lane] == w.blocked_token[lane] &&
                ((state>>(shift%32))&7) == static_cast<uint32_t>(w.state[lane]),"lane transition mismatch");
    }
}
void apply(TestBench& b, simt::Warp& w, const Command& c) {
    simt::Warp expected = w; std::string fault;
    try { expected = reference(w,c); } catch (const std::exception& e) { fault = e.what(); }
    drive(b,w,c);
    static constexpr std::array messages{"", "unsupported_control_opcode", "reconvergence_id_out_of_range",
        "predicated_reconvergence_unsupported", "live_barrier_overwrite", "invalid_reconvergence_target", "bsync_unarmed",
        "bsync_wrong_pc", "barrier_participation_mismatch", "break_unarmed", "branch_target_out_of_range"};
    require(b.dut.error < messages.size() && fault == messages[b.dut.error],"control error/reference differs");
    require(bool(b.dut.accept) == fault.empty(),"control acceptance/reference differs");
    if (fault.empty()) { check_lanes(b,expected); check_tokens(b,expected); w = expected; }
    else { check_lanes(b,w); check_tokens(b,w); }
    b.tick(); b.dut.command_valid = 0; b.dut.eval(); check_tokens(b,w);
}
void nested_wait_break_exit_and_dissolution() {
    TestBench b("control-nested.vcd"); simt::Warp w; Command c;
    apply(b,w,c); c.token = 15; c.target = 40; c.group = c.mask = 0xffff; apply(b,w,c);
    c.op = 27; c.pc = 40; c.group = c.mask = 0xfffe; apply(b,w,c);
    c.op = 28; c.pc = 10; c.group = c.mask = 1; apply(b,w,c);
    require(w.pc[1] == 41 && w.state[1] == simt::LaneState::active,"BREAK did not wake nested join");
    c.op = 27; c.token = 0; c.pc = 64; c.group = c.mask = 0xffff0000; apply(b,w,c);
    c.op = 24; c.pc = 50; c.group = c.mask = 0xffff; apply(b,w,c);
    require(w.pc[31] == 65 && w.state[0] == simt::LaneState::exited,"EXIT did not release outer waiters");
    c = Command{}; c.token = 3; apply(b,w,c); c.op = 28; apply(b,w,c);
    require(w.reconvergence[3].state == simt::TokenState::dissolved,"empty token was not dissolved");
    c.op = 27; c.pc = 64; apply(b,w,c);
}
void all_tokens_and_random_arrival_sequences() {
    TestBench b(nullptr); simt::Warp w; std::mt19937 rng(51630);
    for (uint32_t round = 0; round < 200; ++round) {
        const uint32_t members = uint32_t(rng()) | 1u;
        for (uint32_t id = 0; id < 16; ++id) {
            Command c; c.token = id; c.group = c.mask = members; c.target = 80+id; apply(b,w,c);
        }
        for (uint32_t id = 0; id < 16; ++id) {
            Command c; c.token = id; c.op = 27; c.pc = 80+id;
            c.group = c.mask = members & rng(); apply(b,w,c);
            c.group = c.mask = members & ~c.group; apply(b,w,c);
        }
    }
}
void faults_clear_reuse_and_inactive_commands() {
    TestBench b("control-faults.vcd"); simt::Warp w; Command c; c.token = 15; apply(b,w,c);
    apply(b,w,c); c.guard = 0; apply(b,w,c); c.guard = 7; c.token = 63; apply(b,w,c);
    c.token = 4; c.valid_target = false; apply(b,w,c); c.valid_target = true; c.target = 128; apply(b,w,c);
    c.target = 64; c.op = 27; apply(b,w,c); c.token = 15; c.pc = 63; apply(b,w,c);
    c.op = 28; c.token = 4; apply(b,w,c); c.token = 63; c.mask = 0; apply(b,w,c);
    c.op = 25; c.target = UINT32_MAX; apply(b,w,c); c.mask = 1; apply(b,w,c);
    c.op = 30; apply(b,w,c);
    b.dut.clear = 1; b.dut.eval(); b.tick(); b.dut.clear = 0; b.dut.eval(); w.reconvergence = {}; check_tokens(b,w);
    c = Command{}; c.group = c.mask = 1; apply(b,w,c);
    c.op = 27; c.pc = 64; c.group = c.mask = 2; apply(b,w,c);
    c.op = 28; apply(b,w,c);
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval(); w.reconvergence = {}; check_tokens(b,w);
}
void random_branch_yield_and_preserved_lane_payload() {
    TestBench b(nullptr); simt::Warp w; std::mt19937 rng(9482);
    for (uint32_t step = 0; step < 3000; ++step) {
        for (uint32_t lane = 0; lane < 32; ++lane) {
            w.pc[lane] = rng(); w.blocked_token[lane] = rng(); w.state[lane] = static_cast<simt::LaneState>(rng()%6);
        }
        Command c; c.op = step%2 ? 25 : 29; c.group = rng(); c.mask = c.group & rng(); c.pc = rng(); c.target = rng()%128;
        apply(b,w,c);
    }
}
void run_cases() {
    run_case("control nested waits, BREAK/EXIT wakeup and dissolved tokens",nested_wait_break_exit_and_dissolution);
    run_case("control sixteen stored tokens and independent randomized arrivals",all_tokens_and_random_arrival_sequences);
    run_case("control rejected commands preserve tokens, clear/reset and inactive guards",faults_clear_reuse_and_inactive_commands);
    run_case("control branch/yield transition vectors and preserved lane payload",random_branch_yield_and_preserved_lane_payload);
}
int application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return application(); }

#include "simt/warp_control.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action, std::string_view message) {
    bool failed = false;
    try { action(); } catch (const std::exception& error) { failed = error.what() == message; }
    require(failed,"missing or incorrect control error");
}
ControlCommand arm(uint32_t token = 15) { return {26,token,7,0,UINT32_MAX,UINT32_MAX,64,128,true}; }
ControlCommand join(uint32_t token = 15) { return {27,token,7,64,UINT32_MAX,UINT32_MAX,0,128,false}; }

// Submission observes next state without adding hidden edges; only tick commits token storage.
// 提交观察下一状态而不增加隐藏边沿；只有 tick 提交令牌存储。
void clocks_pending_transaction_and_clear() {
    auto unit = make_warp_control();
    for (uint32_t idle = 0; idle < 100; ++idle) unit->tick();
    require(unit->stats().clocks == 100 && unit->stats().active_clocks == 0,"idle clock gate");
    unit->clear(); rejects([&] { unit->execute(arm(),{}); },"control_command_phase"); unit->tick();
    const auto result = unit->execute(arm(),{});
    require(result.tokens[15].members == UINT32_MAX && result.tokens[15].target == 64,"arm pins");
    rejects([&] { unit->execute(join(),{}); },"control_command_phase");
    rejects([&] { unit->clear(); },"control_clear_after_command");
    require(unit->stats().clocks == 101 && unit->stats().commands == 0,"submission inserted clock"); unit->tick();
    const auto completed = unit->execute(join(),{});
    require(completed.tokens[15].state == 0 && completed.lanes.pc[31] == 65,"stored arm not consumed"); unit->tick();
    const auto stats = unit->stats();
    require(stats.commands == 2 && stats.clears == 1 && stats.woken_lanes == 32 && stats.active_clocks == 3,
            "transaction accounting");
    for (uint32_t idle = 0; idle < 10; ++idle) unit->tick();
    require(unit->stats().active_clocks == 3 && unit->stats().woken_lanes == 32,"repeated stale wake event");
}

// Mutating returned observations cannot rewrite the persistent records or another physical warp.
// 修改返回的观察快照不能改写持久记录或另一个物理 warp。
void all_tokens_observation_isolation_and_reuse() {
    auto first = make_warp_control(), second = make_warp_control();
    for (uint32_t token = 0; token < 16; ++token) {
        auto snapshot = first->execute(arm(token),{}); first->tick();
        snapshot.tokens[token] = {};
        rejects([&] { second->execute(join(token),{}); },"bsync_unarmed");
    }
    for (uint32_t token = 0; token < 16; ++token) {
        auto result = first->execute(join(token),{}); first->tick();
        require(result.lanes.pc[0] == 65 && result.tokens[token].state == 0,"stored token lost");
    }
    first->execute(arm(),{}); first->tick(); first->clear(); first->tick();
    rejects([&] { first->execute(join(),{}); },"bsync_unarmed");
    first->execute(arm(),{}); first->tick(); first->execute(join(),{}); first->tick();
    require(second->stats().commands == 0 && second->stats().clocks == 0,"warp instances shared state");
    require(first->stats().woken_lanes == 17*32,"token clear or reuse leaked wakeups");
}

// Invalid descriptors and rejected RTL transactions preserve the previously armed barrier.
// 非法描述符和被 RTL 拒绝的事务保留先前已设置的屏障。
void rejected_commands_preserve_tokens_and_release_pins() {
    auto unit = make_warp_control(); unit->execute(arm(),{}); unit->tick();
    for (uint32_t token : {16,63,64}) {
        rejects([&] { unit->execute(arm(token),{}); }, token < 64 ? "reconvergence_id_out_of_range" : "control_descriptor");
    }
    auto command = arm(); command.opcode = 256;
    rejects([&] { unit->execute(command,{}); },"control_descriptor");
    command = arm(); command.guard = 16;
    rejects([&] { unit->execute(command,{}); },"control_descriptor");
    ControlLanes lanes; lanes.state[31] = 6;
    rejects([&] { unit->execute(join(),lanes); },"control_lane_state");
    rejects([&] { unit->execute(arm(),{}); },"live_barrier_overwrite");
    command = join(); command.pc = 63;
    rejects([&] { unit->execute(command,{}); },"bsync_wrong_pc");
    require(unit->stats().clocks == 1 && unit->stats().commands == 1,"rejection advanced state");
    require(unit->execute(join(),{}).lanes.pc[31] == 65,"rejection lost armed token"); unit->tick();
    require(unit->stats().commands == 2,"rejection counted as command");
}

// Cover every packed three-bit boundary and preserve the full-width nonparticipating lane payload.
// 覆盖所有三位打包边界，并保留未参与 lane 的完整位宽载荷。
void packed_lane_boundaries_and_branch_masks() {
    auto unit = make_warp_control(); ControlLanes lanes;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        lanes.pc[lane] = 0xabcdef00+lane; lanes.blocked[lane] = 0xfedcba00+lane; lanes.state[lane] = lane%6;
    }
    const ControlCommand branch{25,63,7,7,0x80000003,0x80000001,100,128,false};
    auto result = unit->execute(branch,lanes); unit->tick();
    auto expected_pc = lanes.pc; expected_pc[0] = 100; expected_pc[1] = 8; expected_pc[31] = 100;
    require(result.lanes.pc == expected_pc && result.lanes.state == lanes.state && result.lanes.blocked == lanes.blocked,
            "packed lane payload or branch mask");
    auto yield = branch; yield.opcode = 29; yield.group = UINT32_MAX; yield.mask = 0xaaaaaaaa;
    result = unit->execute(yield,lanes); unit->tick();
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(result.lanes.state[lane] == (lane%2 ? 2 : lanes.state[lane]),"packed yield state");
}

void run_cases() {
    clocks_pending_transaction_and_clear(); std::cout << "PASS control adapter clocks, pending transaction and clear\n";
    all_tokens_observation_isolation_and_reuse(); std::cout << "PASS control adapter token ownership, independent warps and reuse\n";
    rejected_commands_preserve_tokens_and_release_pins(); std::cout << "PASS control adapter rejection isolation and pin withdrawal\n";
    packed_lane_boundaries_and_branch_masks(); std::cout << "PASS control adapter packed lane boundaries and branch masks\n";
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

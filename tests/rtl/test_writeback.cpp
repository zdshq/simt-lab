#include "bench.hpp"
#include "Vwriteback_unit.h"
#include <algorithm>
#include <array>
#include <bit>
#include <random>
#include <vector>

namespace {
constexpr uint32_t count = COLLECTOR_ENTRIES, bits = std::countr_zero(count);
using Vector = std::array<uint32_t,32>;
class TestBench : public Bench<Vwriteback_unit> {
public:
    TestBench() : Bench(nullptr) { dut.port_groups = 4; dut.eval(); }
    void step() {
        tick(); dut.reserve_valid = 0; dut.complete_valid = 0; dut.acknowledge = 0; dut.eval();
    }
};

Vector pattern(uint32_t seed) {
    Vector value;
    for (uint32_t lane = 0; lane < 32; ++lane) value[lane] = seed*1009+lane*17;
    return value;
}

uint32_t reserve(TestBench& b, uint32_t pipe, uint32_t kind, uint32_t reg, uint32_t mask = UINT32_MAX) {
    auto& d = b.dut;
    d.reserve_warp = 3; d.reserve_pipe = pipe; d.reserve_kind = kind;
    d.reserve_register = reg; d.reserve_mask = mask; d.reserve_valid = 1; d.eval();
    require(d.reserve_ready && !d.reserve_error, "writeback reservation rejected");
    return d.reserve_index;
}

void complete(TestBench& b, uint32_t pipe, uint32_t entry, const Vector& data) {
    auto& d = b.dut; const auto shift = pipe*bits;
    d.complete_entries = (d.complete_entries & ~(uint64_t{count-1}<<shift)) | (uint64_t{entry}<<shift);
    for (uint32_t lane = 0; lane < 32; ++lane) d.complete_data[pipe*32+lane] = data[lane];
    d.complete_valid |= 1u<<pipe; d.eval();
}

void expect_data(const Vwriteback_unit& d, uint32_t port, uint32_t entry, const Vector& data) {
    require(d.retire_valid & (1u<<port), "missing retirement port");
    require(((d.retire_entries>>(port*bits)) & (count-1)) == entry, "wrong retirement identity");
    for (uint32_t lane = 0; lane < 32; ++lane)
        require(d.retire_data[port*32+lane] == data[lane], "held/forwarded result data differs");
}

// Retirement may forward a new completion immediately, but acknowledgement cannot open a second grant.
// 退休可以立即旁路新完成结果，但确认不能开启第二次授权。
void simultaneous_results_and_held_payload() {
    TestBench b;
    const auto old = reserve(b,0,1,0,0x80000001); b.step();
    const auto young = reserve(b,1,1,4); b.step();
    const auto pred = reserve(b,2,2,3); b.step();
    complete(b,0,old,pattern(10)); complete(b,1,young,pattern(20)); complete(b,2,pred,pattern(30));
    require(b.dut.complete_accept == 7, "independent producer completions serialized");
    require(b.dut.retire_ready == ((1u<<old)|(1u<<pred)), "bank/predicate arbitration differs");
    expect_data(b.dut,0,old,pattern(10)); expect_data(b.dut,4,pred,pattern(30));
    b.dut.acknowledge = (1u<<old)|(1u<<pred); b.dut.eval();
    require(b.dut.write_valid == 1 && b.dut.write_masks[0] == 0x80000001, "physical write mask differs");
    require(!(b.dut.retire_ready & (1u<<young)), "same bank consumed twice in one cycle");
    b.step();
    for (auto& word : b.dut.complete_data) word = 0xdeadbeef;
    for (uint32_t wait = 0; wait < 12; ++wait) {
        b.dut.eval(); expect_data(b.dut,0,young,pattern(20));
        require(!b.dut.clock_enable && !b.dut.write_valid, "held result caused a write or storage clock"); b.step();
    }
    b.dut.acknowledge = 1u<<young; b.dut.eval(); b.step();
    require(b.dut.occupancy == 0, "completion storage leaked");
}

void capacity_reuse_and_issue_age() {
    TestBench b; std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < count; ++i) { ids.push_back(reserve(b,i%7,1,0)); b.step(); }
    require(!b.dut.reserve_ready && b.dut.occupancy == count, "completion table overcommitted");
    complete(b,0,ids[0],pattern(1)); b.dut.acknowledge = 1u<<ids[0]; b.dut.eval();
    require(reserve(b,6,1,0) == ids[0], "same-cycle capacity was not returned"); b.step();
    complete(b,6,ids[0],pattern(999)); complete(b,1,ids[1],pattern(2));
    expect_data(b.dut,0,ids[1],pattern(2));
    b.dut.acknowledge = 1u<<ids[1]; b.dut.eval(); b.step();
    expect_data(b.dut,0,ids[0],pattern(999));
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval();
    require(!b.dut.occupancy && !b.dut.retire_ready, "reset exposed retained completion data");
}

void rejected_inputs_do_not_corrupt_results() {
    TestBench b;
    complete(b,0,0,pattern(1)); require(b.dut.complete_error == 1, "unreserved completion accepted"); b.step();
    b.dut.reserve_kind = 3; b.dut.reserve_valid = 1; b.dut.eval();
    require(b.dut.reserve_error && !b.dut.reserve_ready, "invalid destination accepted"); b.step();
    b.dut.reserve_kind = 2; b.dut.reserve_register = 7; b.dut.reserve_valid = 1; b.dut.eval();
    require(b.dut.reserve_error && !b.dut.reserve_ready, "constant predicate accepted"); b.step();
    const auto id = reserve(b,1,1,1); b.step();
    complete(b,0,id,pattern(2)); require(b.dut.complete_error == 1, "wrong producer accepted"); b.step();
    b.dut.acknowledge = 1u<<id; b.dut.eval(); require(b.dut.acknowledge_error, "unfinished result retired"); b.step();
    complete(b,1,id,pattern(3)); b.step();
    complete(b,1,id,pattern(4)); require(b.dut.complete_error == 2, "duplicate completion accepted");
    expect_data(b.dut,1,id,pattern(3)); b.step(); expect_data(b.dut,1,id,pattern(3));
}

void all_producer_ports_and_destination_types() {
    TestBench b; std::array<uint32_t,7> ids{};
    const auto ports = std::min(count,7u);
    for (uint32_t port = 0; port < ports; ++port) {
        const uint32_t kind = port < 4 || port == 6 ? 1 : port == 4 ? 2 : 0;
        ids[port] = reserve(b,port,kind,port == 6 ? 63 : port); b.step();
    }
    uint32_t entries = 0;
    for (uint32_t port = 0; port < ports; ++port) {
        complete(b,port,ids[port],pattern(port+10)); entries |= 1u<<ids[port];
    }
    require(b.dut.complete_accept == (1u<<ports)-1 && b.dut.retire_ready == entries,
            "independent seven producer ports or no-write retirements lost");
    for (uint32_t port = 0; port < std::min(ports,5u); ++port)
        expect_data(b.dut,port,ids[port],pattern(port+10));
    b.dut.acknowledge = entries; b.dut.eval();
    require(b.dut.write_valid == 15,"predicate or R63 consumed a GPR bank"); b.step();
    require(!b.dut.occupancy,"parallel retirements leaked reserved entries");
}

struct Reference {
    bool live = false, done = false;
    uint32_t age = 0, pipe = 0, kind = 0, reg = 0, mask = 0;
    Vector data{};
};
using State = std::array<Reference,count>;

uint32_t expected_grants(const State& state, uint32_t groups) {
    uint32_t ready = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& r = state[index]; if (!r.live || !r.done) continue;
        bool selected = true;
        if (r.kind) for (const auto& other : state)
            if (other.live && other.done && other.kind == r.kind && other.age < r.age &&
                (r.kind == 2 || other.reg%groups == r.reg%groups)) selected = false;
        if (selected) ready |= 1u<<index;
    }
    return ready;
}

void verify_outputs(const Vwriteback_unit& d, const State& state) {
    const auto grants = expected_grants(state,d.port_groups);
    require(d.retire_ready == grants, "random arbitration differs from issue-order reference");
    uint32_t writes = 0;
    for (uint32_t i = 0; i < count; ++i) if ((grants & (1u<<i)) && state[i].kind) {
        const auto& r = state[i]; const auto port = r.kind == 2 ? 4 : r.reg%4;
        expect_data(d,port,i,r.data);
        if (port < 4 && (d.acknowledge & (1u<<i))) {
            writes |= 1u<<port;
            require(d.write_masks[port] == r.mask, "random physical write mask differs");
            require(((d.write_rows>>(port*7)) & 127u) == 3*16+r.reg/4, "random physical write row differs");
        }
    }
    require(d.write_valid == writes, "random unacknowledged or missing physical RF write");
}

void random_completions(TestBench& b, State& state, std::mt19937& random) {
    for (uint32_t port = 0; port < 7; ++port) {
        if (random()%3 == 0) continue;
        for (uint32_t index = 0; index < count; ++index) {
            auto& r = state[index]; if (!r.live || r.done || r.pipe != port) continue;
            complete(b,port,index,r.data); require(!(b.dut.complete_error & (1u<<port)), "legal random completion rejected");
            r.done = true; break;
        }
    }
}

// The independent reference stores monotonic issue IDs rather than the circuit's compact ranks.
// 独立参考保存单调发射编号，不使用电路的压紧年龄表示。
void randomized_completion_and_port_groups() {
    TestBench b; State state{}; std::mt19937 random(20261030); uint32_t age = 0;
    for (uint32_t cycle = 0; cycle < 3000; ++cycle) {
        b.dut.port_groups = 1u<<(random()%3); b.dut.eval();
        random_completions(b,state,random); verify_outputs(b.dut,state);
        b.dut.acknowledge = expected_grants(state,b.dut.port_groups) & random(); b.dut.eval();
        verify_outputs(b.dut,state);
        for (uint32_t i = 0; i < count; ++i) if (b.dut.acknowledge & (1u<<i)) state[i].live = false;
        if (b.dut.reserve_ready && random()%5 != 0) {
            const uint32_t pipe = random()%7, kind = random()%3, reg = random()%(kind == 2 ? 7 : 64);
            const uint32_t mask = random()%5 == 0 ? 0 : random();
            const auto id = reserve(b,pipe,kind,reg,mask);
            require(!state[id].live, "reservation reused live storage");
            const uint32_t normalized = !mask || (kind == 1 && reg == 63) ? 0 : kind;
            state[id] = {true,false,++age,pipe,normalized,reg,mask,pattern(age)};
        }
        b.step(); uint32_t live = 0; for (const auto& r : state) live += r.live;
        require(b.dut.occupancy == live, "random completion occupancy differs");
    }
}

void run_cases() {
    run_case("writeback simultaneous results, masked RF writes and held payload",simultaneous_results_and_held_payload);
    run_case("writeback capacity, same-cycle reuse, issue age and reset",capacity_reuse_and_issue_age);
    run_case("writeback malformed ownership and duplicate completion rejection",rejected_inputs_do_not_corrupt_results);
    run_case("writeback seven producer ports, predicates and no-write destinations",all_producer_ports_and_destination_types);
    run_case("writeback 3000 random completion cycles across 1/2/4 port groups",randomized_completion_and_port_groups);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

#include "Vexecution_arbiter.h"
#include "bench.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <optional>
#include <random>

namespace {
uint64_t port(uint32_t pipe,uint32_t part) { return uint64_t{1}<<(pipe*8+part); }
struct Driver {
    Bench<Vexecution_arbiter> b{nullptr};
    Driver() { b.dut.partition_limit=8; b.dut.integer_interval=1; b.dut.floating_interval=1; b.dut.special_interval=1; b.dut.eval(); }
    void edge() {
        b.tick(); auto& d=b.dut; d.requests=0; d.accepts=0; d.async_noop=0; d.async_busy=0; d.tensor_release=0; d.eval();
    }
    uint64_t request(uint64_t mask) { b.dut.requests=mask; b.dut.eval(); return b.dut.grants; }
    void accept(uint64_t mask) { b.dut.accepts=mask; b.dut.eval(); require(!b.dut.error,"legal acceptance rejected"); }
};

void private_intervals_and_full_width() {
    Driver t; auto& d=t.b.dut; d.integer_interval=1; d.floating_interval=3; d.special_interval=101;
    constexpr uint64_t mask=0xffffff;
    require(t.request(mask)==mask,"initial private ports unavailable"); t.accept(mask); t.edge();
    for (uint32_t cycle=1; cycle<=101; ++cycle) {
        const auto expected=uint64_t{255} | (cycle>=3 ? uint64_t{255}<<8 : 0) | (cycle>=101 ? uint64_t{255}<<16 : 0);
        require(t.request(mask)==expected,"interval off by one or missing physical partition"); t.edge();
    }
    d.special_interval=UINT32_MAX; require(t.request(port(2,7))==port(2,7),"highest port unavailable");
    t.accept(port(2,7)); t.edge(); require(d.cooldowns[23]==UINT32_MAX-1,"32-bit interval truncated");
    for (uint32_t i=0; i<9; ++i) t.edge();
    require(d.cooldowns[23]==UINT32_MAX-10 && d.clock_enable,"idle cycles failed to advance cooldown");
}

void independent_shared_rotation_and_backpressure() {
    Driver t; auto& d=t.b.dut; constexpr uint64_t requests=uint64_t{65535}<<40;
    for (uint32_t n=0; n<24; ++n) {
        if (n) { d.tensor_release=1; d.tensor_release_owner=(n-1)%8; }
        const auto expected=port(5,n%8)|port(6,n%8);
        require(t.request(requests)==expected,"shared round-robin or retirement bypass failed");
        require(std::popcount(uint64_t(d.arbitration_stalls))==14,"losing partition count wrong");
        // Refusing a downstream grant does not consume priority or release the selected source data.
        // 下游拒绝授权不会消耗优先级，也不会释放所选源数据。
        if (n==0) { t.edge(); require(t.request(requests)==expected && !d.tensor_busy,"unaccepted grant changed state"); }
        t.accept(expected); t.edge(); require(d.tensor_busy && d.tensor_owner==n%8,"Tensor owner not retained");
    }
    d.tensor_release=1; d.tensor_release_owner=7; t.edge(); require(!d.tensor_busy,"Tensor release lost");
}

void async_noops_busy_and_separate_ports() {
    Driver t; auto& d=t.b.dut;
    t.request(port(5,7)); t.accept(port(5,7)); t.edge();
    d.async_busy=1; d.async_noop=0x81;
    const uint64_t requests=(uint64_t{1}<<56)-1;
    const auto expected=((uint64_t{1}<<40)-1)|(uint64_t{0x81}<<48);
    require(t.request(requests)==expected,"shared busy blocked private/no-op ports or leaked shared ownership");
    require(d.shared_stalls==((uint64_t{255}<<40)|(uint64_t{0x7e}<<48)),"shared stall causes wrong");
    t.accept(expected); t.edge(); require(d.async_cursor==0 && d.tensor_owner==7,"no-op rotated shared control priority");
    d.async_noop=0x81; require(t.request(uint64_t{255}<<48)==((uint64_t{0x83})<<48),"no-op competed with actual control request");
    t.accept(uint64_t{0x83}<<48); t.edge(); require(d.async_cursor==2,"actual async handshake not selected");
}

void invalid_batches_and_reset() {
    Driver t; auto& d=t.b.dut; d.integer_interval=10;
    t.request(port(0,0)|port(5,3)); t.accept(port(0,0)|port(5,3)); t.edge();
    d.tensor_release=1; d.tensor_release_owner=2; d.requests=port(0,1); d.accepts=port(0,1); d.eval();
    require(d.error==4 && !d.grants,"wrong owner release accepted"); t.edge();
    require(d.tensor_busy && d.tensor_owner==3 && d.cooldowns[0]==9 && !d.cooldowns[1],"invalid batch partially committed");
    d.partition_limit=4; d.requests=port(4,7); d.eval(); require(d.error==2 && !d.grants,"quota request accepted"); t.edge();
    d.async_noop=1; d.eval(); require(d.error==3,"orphan noop accepted"); t.edge();
    d.accepts=port(0,0); d.eval(); require(d.error==5,"ungranted acceptance allowed"); t.edge();
    d.special_interval=0; d.eval(); require(d.error==1,"zero interval accepted"); t.edge();
    d.special_interval=1; d.reset=1; t.edge(); d.reset=0; d.eval();
    require(!d.tensor_busy && !d.tensor_cursor && !d.async_cursor,"reset retained ownership");
    for (auto value : d.cooldowns) require(!value,"reset retained cooldown");
}

struct Reference {
    uint64_t cycle=0;
    std::array<uint64_t,24> next{};
    std::deque<uint32_t> tensor{0,1,2,3,4,5,6,7}, asynchronous{0,1,2,3,4,5,6,7};
    std::optional<uint32_t> owner;
};
void rotate(std::deque<uint32_t>& order,uint32_t chosen) {
    while (order.front()!=chosen) { order.push_back(order.front()); order.pop_front(); }
    order.push_back(order.front()); order.pop_front();
}
uint64_t shared_choice(uint32_t pipe,uint32_t requests,const std::deque<uint32_t>& order) {
    for (auto part : order) if (requests & (1u<<part)) return port(pipe,part);
    return 0;
}
uint64_t expected_grants(const Reference& r,const Vexecution_arbiter& d) {
    uint64_t grants=d.requests & ((uint64_t{65535})<<24);
    for (uint32_t at=0; at<24; ++at) if (r.cycle>=r.next[at]) grants|=d.requests & (uint64_t{1}<<at);
    if (!r.owner) grants|=shared_choice(5,(d.requests>>40)&255,r.tensor);
    grants|=d.requests & (uint64_t(d.async_noop)<<48);
    if (!d.async_busy) grants|=shared_choice(6,((d.requests>>48)&255)&~d.async_noop,r.asynchronous);
    return grants;
}
void advance_reference(Reference& r,const Vexecution_arbiter& d) {
    for (uint32_t at=0; at<24; ++at) if (d.accepts & (uint64_t{1}<<at))
        r.next[at]=r.cycle+(at<8 ? d.integer_interval : at<16 ? d.floating_interval : d.special_interval);
    for (uint32_t part=0; part<8; ++part) {
        if (d.accepts & port(5,part)) { r.owner=part; rotate(r.tensor,part); }
        if ((d.accepts & port(6,part)) && !(d.async_noop & (1u<<part))) rotate(r.asynchronous,part);
    }
    ++r.cycle;
}

// Absolute deadlines and independent priority queues serve as the oracle for hardware counters and cursors.
// 绝对截止时间与独立优先队列作为硬件计数器和游标的参考。
void random_concurrent_requests() {
    Driver t; auto& d=t.b.dut; Reference r; std::mt19937 rng(176921);
    d.integer_interval=3; d.floating_interval=7; d.special_interval=13;
    for (uint32_t cycle=0; cycle<12000; ++cycle) {
        if (r.owner && rng()%4==0) { d.tensor_release=1; d.tensor_release_owner=*r.owner; r.owner.reset(); }
        d.requests=((uint64_t(rng())<<32)|rng())&((uint64_t{1}<<56)-1);
        d.async_noop=rng()&(d.requests>>48)&255; d.async_busy=rng()%3==0; d.eval();
        const auto expected=expected_grants(r,d); require(d.grants==expected && !d.error,"random grants differ");
        const auto stalls=d.interval_stalls|d.shared_stalls|d.arbitration_stalls;
        require((stalls|d.grants)==d.requests && !(stalls & d.grants),"request lost or doubly classified");
        t.accept(expected & ((uint64_t(rng())<<32)|rng())); advance_reference(r,d); t.edge();
        require(bool(d.tensor_busy)==bool(r.owner) && (!r.owner || d.tensor_owner==*r.owner),"random Tensor lease differs");
        require(d.tensor_cursor==r.tensor.front() && d.async_cursor==r.asynchronous.front(),"random priority differs");
        for (uint32_t at=0; at<24; ++at)
            require(d.cooldowns[at]==(r.next[at]>r.cycle ? r.next[at]-r.cycle : 0),"random cooldown differs");
    }
}
int run_application() {
    try {
        run_case("execution private intervals, eight partitions and full 32-bit counter",private_intervals_and_full_width);
        run_case("execution shared rotation, downstream refusal and Tensor retirement bypass",independent_shared_rotation_and_backpressure);
        run_case("execution async no-ops, shared busy and independent physical ports",async_noops_busy_and_separate_ports);
        run_case("execution invalid batch atomicity, quota, ownership and reset",invalid_batches_and_reset);
        run_case("execution 12000 randomized concurrent requests and independent deadlines",random_concurrent_requests); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

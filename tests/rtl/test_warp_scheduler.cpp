#include "Vwarp_scheduler.h"
#include "bench.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace {
using TestBench=Bench<Vwarp_scheduler>;
uint32_t bit(uint32_t slot) { return uint32_t{1}<<slot; }
template<class Words> uint32_t count(const Words& words,uint32_t slot) {
    uint32_t value=0;
    for (uint32_t i=0; i<6; ++i) value|=((words[(slot*6+i)/32]>>((slot*6+i)%32))&1)<<i;
    return value;
}
template<class Words> void set_count(Words& words,uint32_t slot,uint32_t value) {
    for (uint32_t i=0; i<6; ++i) {
        const auto at=slot*6+i, mask=bit(at%32);
        words[at/32]=(words[at/32]&~mask)|(((value>>i)&1)<<(at%32));
    }
}
struct Driver {
    TestBench b{nullptr};
    Driver() { b.dut.slot_limit=32; b.dut.collector_ready=1; b.dut.completion_ready=1; b.dut.eval(); }
    void edge() {
        b.tick(); auto& d=b.dut; d.allocate_mask=0; d.release_mask=0; d.retire_boundaries=0; d.retire_memories=0;
        d.decision_valid=0; for (auto& w : d.retire_counts) w=0; d.eval();
    }
    bool decide(bool guard=true,bool dependencies=true,bool boundary=false,bool memory=false) {
        auto& d=b.dut; d.guard_ready=guard; d.dependencies_ready=dependencies;
        d.instruction_boundary=boundary; d.instruction_memory=memory; d.decision_valid=1; d.eval(); return d.issue_accept;
    }
    void allocate(uint32_t mask) { b.dut.allocate_mask=mask; b.dut.eval(); require(!b.dut.error,"allocation rejected"); edge(); }
};
void full_slots_counts_and_limits() {
    Driver t; auto& d=t.b.dut; t.allocate(UINT32_MAX); d.runnable=UINT32_MAX; d.eval();
    for (uint32_t n=0; n<1024; ++n) {
        require(d.candidate_valid && d.candidate==n%32,"round-robin lost a physical slot");
        require(t.decide(),"valid in-flight count rejected"); t.edge();
    }
    require(d.total_inflight==1024,"total count truncated");
    for (uint32_t slot=0; slot<32; ++slot) require(count(d.inflight_counts,slot)==32,"highest/full slot count incorrect");
    require(!t.decide() && d.rejection==5,"in-flight overflow accepted"); t.edge();
    for (uint32_t slot=0; slot<32; ++slot) set_count(d.retire_counts,slot,32);
    d.eval(); require(d.total_inflight==0,"retirement bypass missing"); t.edge();
    d.release_mask=UINT32_MAX; d.eval(); require(!d.error,"empty slots not releasable"); t.edge();
    require(!d.live_mask && !d.total_inflight && !d.candidate_valid,"release leaked ownership");
}
void fixed_priority_retries_and_backpressure() {
    Driver t; auto& d=t.b.dut; t.allocate(bit(0)|bit(1)|bit(30)|bit(31)); d.runnable=d.live_mask;
    for (uint32_t policy : {1u,2u}) {
        d.policy=policy; d.eval(); const auto first=policy==1 ? 0u : 31u, second=policy==1 ? 1u : 30u;
        require(d.candidate_valid && d.candidate==first,"fixed priority differs");
        require(!t.decide(false),"guard dependency ignored"); t.edge();
        require(d.candidate==second,"blocked priority candidate starved successor");
        require(t.decide(),"successor rejected"); t.edge(); require(d.candidate==first,"accepted issue did not restart priority search");
    }
    const uint32_t cursor=d.cursor, skipped=d.skipped;
    d.completion_ready=0; d.eval(); require(!d.candidate_valid && d.completion_stall,"completion capacity ignored");
    for (uint32_t i=0; i<7; ++i) t.edge();
    require(d.cursor==cursor && d.skipped==skipped,"backpressure changed priority");
    d.completion_ready=1; d.collector_ready=0; d.eval(); require(!d.candidate_valid && d.collector_stall,"collector capacity ignored");
    d.collector_ready=1; d.eval();
    for (uint32_t i=0; i<8; ++i) { require(d.candidate_valid,"all-rejected sweep stopped"); require(!t.decide(true,false),"dependency ignored"); t.edge(); }
}
void retirement_bypass_and_serialization() {
    Driver t; auto& d=t.b.dut; t.allocate(bit(31)); d.runnable=bit(31); d.eval();
    require(t.decide(true,true,false,true),"initial memory rejected"); t.edge();
    require(t.decide(),"memory blocked independent arithmetic"); t.edge();
    require(!t.decide(true,true,false,true) && d.rejection==3,"second memory bypassed order"); t.edge();
    require(!t.decide(true,true,true,false) && d.rejection==2,"boundary bypassed pending instructions"); t.edge();
    set_count(d.retire_counts,31,2); d.retire_memories=bit(31); d.eval();
    require(t.decide(true,true,true,false),"same-cycle final retirement did not enable boundary"); t.edge();
    require(d.boundary_mask==bit(31) && count(d.inflight_counts,31)==1 && !d.candidate_valid,"boundary did not serialize warp");
    set_count(d.retire_counts,31,1); d.retire_boundaries=bit(31); d.eval();
    require(t.decide(true,true,false,true),"boundary retirement bypass failed"); t.edge();
    require(!d.boundary_mask && d.memory_mask==bit(31) && count(d.inflight_counts,31)==1,"retire/issue flags overlapped incorrectly");
}
void protocol_errors_slot_reuse_and_reset() {
    Driver t; auto& d=t.b.dut; t.allocate(bit(7)); d.runnable=bit(7); d.eval(); require(t.decide(),"issue failed"); t.edge();
    d.release_mask=bit(7); d.eval(); require(d.error==4 && !d.candidate_valid,"busy release accepted"); t.edge();
    require(d.live_mask==bit(7) && count(d.inflight_counts,7)==1,"rejected release mutated state");
    set_count(d.retire_counts,7,2); d.allocate_mask=bit(8); d.eval(); require(d.error==2,"underflow accepted"); t.edge();
    require(d.live_mask==bit(7) && count(d.inflight_counts,7)==1,"error did not reject the whole event batch");
    set_count(d.retire_counts,7,1); d.release_mask=bit(7); d.allocate_mask=bit(7); d.eval();
    require(!d.error && d.live_mask==bit(7) && count(d.inflight_counts,7)==0,"same-edge ownership replacement failed"); t.edge();
    d.allocate_mask=bit(7); d.eval(); require(d.error==5,"live allocation accepted"); t.edge();
    d.slot_limit=8; d.runnable=bit(31); d.eval(); require(d.error==6,"runtime slot quota ignored");
    d.runnable=0; d.policy=3; d.eval(); require(d.error==1,"invalid policy accepted"); d.policy=0;
    d.reset=1; t.edge(); d.reset=0; d.eval(); require(!d.live_mask && !d.total_inflight && !d.skipped && !d.cursor,"reset retained scheduling state");
}

struct Slot { bool live=false,boundary=false,memory=false; uint32_t count=0; };
struct Oracle { std::array<Slot,32> slots{}; std::set<uint32_t> skipped; uint32_t cursor=0; };
std::optional<uint32_t> candidate(Oracle& o,uint32_t runnable,uint32_t policy) {
    std::vector<uint32_t> eligible,untried;
    for (uint32_t i=0; i<32; ++i) if ((runnable&bit(i)) && o.slots[i].live && !o.slots[i].boundary) {
        eligible.push_back(i); if (!o.skipped.contains(i)) untried.push_back(i);
    }
    if (untried.empty()) untried=eligible;
    if (untried.empty()) return {};
    if (policy==1) return untried.front();
    if (policy==2) return untried.back();
    const auto next=std::lower_bound(untried.begin(),untried.end(),o.cursor);
    return next==untried.end() ? untried.front() : *next;
}
void lifecycle(Driver& t,Oracle& o,std::mt19937& rng) {
    auto& d=t.b.dut;
    for (uint32_t i=0; i<32; ++i) {
        auto& s=o.slots[i];
        if (s.count && rng()%5==0) {
            set_count(d.retire_counts,i,s.count);
            if (s.boundary) d.retire_boundaries|=bit(i);
            if (s.memory) d.retire_memories|=bit(i);
            s.count=0; s.boundary=false; s.memory=false;
        }
        if (!s.count && s.live && rng()%31==0) { d.release_mask|=bit(i); s.live=false; o.skipped.erase(i); }
        if (!s.live && rng()%9==0) { d.allocate_mask|=bit(i); s.live=true; o.skipped.erase(i); }
    }
}
void compare(const Driver& t,const Oracle& o) {
    uint32_t live=0,boundary=0,memory=0,total=0,skipped=0;
    for (uint32_t i=0; i<32; ++i) {
        const auto& s=o.slots[i]; if (s.live) live|=bit(i); if (s.boundary) boundary|=bit(i); if (s.memory) memory|=bit(i);
        total+=s.count; require(count(t.b.dut.inflight_counts,i)==s.count,"random in-flight count mismatch");
    }
    for (auto i : o.skipped) skipped|=bit(i);
    const auto& d=t.b.dut; require(d.live_mask==live && d.boundary_mask==boundary && d.memory_mask==memory &&
        d.total_inflight==total && d.cursor==o.cursor && d.skipped==skipped && !d.error,"random scheduler state mismatch");
}
void random_decision(Driver& t,Oracle& o,std::mt19937& rng) {
    auto& d=t.b.dut; d.runnable=rng(); d.policy=rng()%3; d.collector_ready=rng()%4!=0; d.completion_ready=rng()%7!=0; d.eval();
    const auto expected=candidate(o,d.runnable,d.policy);
    require(bool(d.candidate_valid)==bool(expected && d.collector_ready && d.completion_ready),"random readiness mismatch");
    if (!d.candidate_valid || rng()%7==0) return;
    require(d.candidate==*expected,"random selection mismatch"); auto& s=o.slots[*expected];
    const bool guard=rng()%4!=0, deps=rng()%3!=0, boundary=rng()%7==0, memory=rng()%5==0;
    const bool accepted=guard && deps && !(boundary && s.count) && !(memory && s.memory) && s.count<32;
    bool untried=false;
    for (uint32_t i=0; i<32; ++i) if ((d.runnable&bit(i)) && o.slots[i].live && !o.slots[i].boundary && !o.skipped.contains(i)) untried=true;
    require(t.decide(guard,deps,boundary,memory)==accepted,"random admission mismatch");
    if (accepted) { ++s.count; s.boundary|=boundary; s.memory|=memory; o.skipped.clear(); }
    else { if (!untried) o.skipped.clear(); o.skipped.insert(*expected); }
    o.cursor=(*expected+1)%32;
}
void randomized_lifecycle_and_decisions() {
    Driver t; Oracle o; std::mt19937 rng(479013);
    for (uint32_t i=0; i<12000; ++i) { lifecycle(t,o,rng); random_decision(t,o,rng); t.edge(); compare(t,o); }
}
int run_application() {
    try {
        run_case("scheduler full 32 slots, 32 in-flight credits each and simultaneous retirement",full_slots_counts_and_limits);
        run_case("scheduler fixed priorities, rejected sweeps and capacity backpressure",fixed_priority_retries_and_backpressure);
        run_case("scheduler boundary/memory ordering and same-cycle retirement bypass",retirement_bypass_and_serialization);
        run_case("scheduler atomic errors, quota, slot replacement and reset",protocol_errors_slot_reuse_and_reset);
        run_case("scheduler 12000 randomized lifecycle and decision cycles",randomized_lifecycle_and_decisions); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

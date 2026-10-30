#include "simt/warp_scheduler.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool failed=false; try { action(); } catch (const std::exception& e) { failed=e.what()==expected; }
    require(failed,"wrong or missing scheduler exception");
}
void issue(WarpScheduler& unit,uint32_t slot,bool boundary=false,bool memory=false) {
    const auto selected=unit.probe(uint32_t{1}<<slot,0,true,true);
    require(selected && *selected==slot,"expected candidate unavailable");
    require(unit.decide(true,true,boundary,memory),"expected issue rejected"); unit.tick();
}
void explicit_clocks_and_snapshot_isolation() {
    auto unit=make_warp_scheduler(32); unit->allocate(0); unit->allocate(31);
    require(!unit->stats().clocks && !unit->stats().admitted,"allocation advanced hidden clocks");
    require(unit->probe(0x80000001,0,true,true)==0,"lowest initial candidate wrong");
    require(unit->decide(true,true,false,false),"new owner cannot issue");
    rejects([&]{unit->probe(1,0,true,true);},"scheduler_probe_after_decision");
    rejects([&]{unit->release(0);},"scheduler_event_after_decision");
    rejects([&]{unit->allocate(1);},"scheduler_event_after_decision");
    rejects([&]{unit->retire(0,false,false);},"scheduler_event_after_decision"); unit->tick();
    const auto clocks=unit->stats().clocks;
    for (uint32_t i=0; i<20; ++i) {
        auto snapshot=unit->read(); snapshot.inflight[0]=999;
        require(unit->read().inflight[0]!=snapshot.inflight[0],"snapshot mutated scheduler storage");
        require(unit->probe(0x80000001,0,true,true)==31,"observation changed cursor");
    }
    require(unit->stats().clocks==clocks && unit->stats().issued==1,"query advanced scheduler clocks");
    rejects([&]{unit->release(0);},"scheduler_invalid_release");
    unit->retire(0,false,false); unit->release(0); unit->release(31);
    require(unit->read().live==0 && unit->stats().released==0,"release bypass or explicit clock differs"); unit->tick();
    require(unit->stats().admitted==2 && unit->stats().released==2 && unit->stats().retired==1,"lifecycle events lost");
    const auto active=unit->stats().active_clocks; unit->tick(); require(unit->stats().active_clocks==active,"idle clock not gated");
}
void serialization_and_retirement_bypass() {
    auto unit=make_warp_scheduler(32); unit->allocate(31); unit->tick();
    issue(*unit,31,false,true); issue(*unit,31);
    require(unit->probe(0x80000000,0,true,true)==31 && !unit->decide(true,true,false,true),"pending memory ignored"); unit->tick();
    require(unit->probe(0x80000000,0,true,true)==31 && !unit->decide(true,true,true,false),"boundary issued with older work"); unit->tick();
    unit->retire(31,false,true); unit->retire(31,false,false); issue(*unit,31,true,false);
    require(!unit->probe(0x80000000,0,true,true),"active boundary did not block fetch");
    unit->retire(31,true,false); issue(*unit,31,false,true);
    const auto s=unit->read(); require(s.inflight[31]==1 && s.memory==0x80000000 && !s.boundary,"retire/issue flag bypass differs");
    unit->retire(31,false,true); unit->release(31); unit->tick();
    require(unit->stats().issued==4 && unit->stats().retired==4 && unit->stats().boundary_stalls==1 && unit->stats().memory_stalls==1,"serialization event accounting");
}
void independent_instances_and_rejected_events() {
    rejects([]{make_warp_scheduler(0);},"invalid_scheduler_slots");
    rejects([]{make_warp_scheduler(33);},"invalid_scheduler_slots");
    auto a=make_warp_scheduler(32),b=make_warp_scheduler(4);
    for (uint32_t slot=0; slot<32; ++slot) a->allocate(slot);
    a->tick(); b->allocate(0); b->tick();
    require(a->read().live==0xffffffff && b->read().live==1,"full geometry or instance isolation failed");
    rejects([&]{b->allocate(4);},"scheduler_slot_out_of_range");
    rejects([&]{b->probe(16,0,true,true);},"invalid_scheduler_probe");
    rejects([&]{b->allocate(0);},"scheduler_invalid_allocation");
    rejects([&]{b->retire(0,false,false);},"scheduler_invalid_retirement");
    issue(*b,0); b->retire(0,false,false);
    rejects([&]{b->retire(0,false,false);},"scheduler_invalid_retirement");
    b->release(0); b->allocate(0); b->tick();
    require(b->read().inflight[0]==0 && b->read().live==1 && !a->stats().issued,"rejected event changed valid batch or other instance");
}
void priority_sweeps_and_shared_capacity() {
    auto unit=make_warp_scheduler(32);
    for (uint32_t slot : {0u,1u,30u,31u}) unit->allocate(slot);
    unit->tick();
    for (uint32_t policy : {0u,1u,2u}) {
        uint32_t visited=0;
        for (uint32_t i=0; i<4; ++i) {
            const auto slot=unit->probe(0xc0000003,policy,true,true); require(slot && !(visited&(uint32_t{1}<<*slot)),"rejected candidate repeated before sweep");
            visited|=uint32_t{1}<<*slot; require(!unit->decide(false,false,false,false),"guard rejection failed"); unit->tick();
        }
        require(visited==0xc0000003,"priority sweep missed a candidate");
    }
    require(!unit->probe(0xc0000003,0,false,true),"collector pressure ignored"); unit->tick();
    require(!unit->probe(0xc0000003,0,true,false),"writeback pressure ignored"); unit->tick();
    const auto& s=unit->stats();
    require(s.probes==12 && !s.issued && s.guard_stalls==12 && s.collector_stalls==1 && s.completion_stalls==1,"probe/stall event accounting");
}
void run_cases() {
    explicit_clocks_and_snapshot_isolation(); std::cout<<"PASS scheduler adapter explicit clocks, snapshots and ownership release\n";
    serialization_and_retirement_bypass(); std::cout<<"PASS scheduler adapter boundary/memory order and retirement bypass\n";
    independent_instances_and_rejected_events(); std::cout<<"PASS scheduler adapter full slots, independent instances and rejected events\n";
    priority_sweeps_and_shared_capacity(); std::cout<<"PASS scheduler adapter three priority sweeps and finite shared capacity\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

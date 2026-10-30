#include "simt/execution_arbiter.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
uint64_t port(uint32_t pipe,uint32_t part) { return uint64_t{1}<<(pipe*8+part); }
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool caught=false; try { action(); } catch (const std::exception& e) { caught=e.what()==expected; }
    require(caught,"wrong or missing arbiter exception");
}
void clocks_snapshots_and_geometries() {
    for (uint32_t parts : {1u,2u,4u,8u}) {
        auto unit=make_execution_arbiter(parts,{3,5,7}); const auto mask=(uint64_t{1}<<parts)-1;
        require(unit->arbitrate(mask,0,false)==mask,"partition quota lost candidate");
        for (uint32_t part=0; part<parts; ++part) unit->accept(0,part);
        require(!unit->stats().clocks && !unit->stats().accepted[0],"accept advanced hidden clocks"); unit->tick();
        for (uint32_t query=0; query<20; ++query) {
            auto state=unit->read(); state.cooldowns[parts-1]=999;
            require(unit->read().cooldowns[parts-1]!=state.cooldowns[parts-1],"snapshot mutated hardware");
        }
        require(unit->stats().clocks==1 && unit->stats().accepted[0]==parts,"snapshot changed events");
        unit->tick(); unit->tick(); const auto active=unit->stats().active_clocks; unit->tick();
        require(unit->stats().active_clocks==active,"idle arbitration clock not gated");
    }
}
void tensor_lease_and_same_cycle_handoff() {
    auto unit=make_execution_arbiter(8,{1,1,1});
    require(unit->arbitrate(uint64_t{255}<<40,0,false)==port(5,0),"initial shared priority wrong");
    unit->accept(5,0); unit->tick();
    require(unit->read().tensor_busy && unit->read().tensor_owner==0,"Tensor ownership missing");
    rejects([&]{unit->release_tensor(1);},"execution_arbiter_invalid_release");
    unit->release_tensor(0); require(!unit->read().tensor_busy,"release bypass missing");
    require(unit->arbitrate(uint64_t{255}<<40,0,false)==port(5,1),"handoff not round-robin");
    rejects([&]{unit->release_tensor(0);},"execution_arbiter_release_phase");
    unit->accept(5,1); unit->tick(); unit->release_tensor(1); unit->tick();
    require(!unit->read().tensor_busy && unit->stats().tensor_releases==2 && unit->stats().accepted[5]==2,"lease counts differ");
}
void downstream_refusal_async_and_intervals() {
    auto unit=make_execution_arbiter(8,{1,3,4});
    unit->arbitrate(port(6,7),0,false); unit->tick();
    require(unit->read().async_cursor==0 && unit->stats().downstream_stalls[6]==1,"refused grant changed priority");
    require(unit->arbitrate(uint64_t{255}<<48,0x80,true)==port(6,7),"no-op blocked by shared control");
    unit->accept(6,7); unit->tick(); require(unit->read().async_cursor==0,"no-op consumed actual shared port");
    unit->arbitrate(port(1,7)|port(6,7),0,false); unit->accept(1,7); unit->accept(6,7); unit->tick();
    require(unit->read().cooldowns[15]==2,"floating counter not loaded");
    for (uint32_t i=0; i<2; ++i) { require(!unit->arbitrate(port(1,7),0,false),"interval not enforced"); unit->tick(); }
    require(unit->arbitrate(port(1,7),0,false)==port(1,7),"interval blocked too long"); unit->accept(1,7); unit->tick();
    require(unit->stats().interval_stalls[1]==2 && unit->stats().shared_stalls[6]==7,"stall events wrong");
}
void invalid_events_phases_and_isolation() {
    rejects([]{make_execution_arbiter(9,{1,1,1});},"invalid_execution_arbiter_config");
    rejects([]{make_execution_arbiter(1,{0,1,1});},"invalid_execution_arbiter_config");
    auto unit=make_execution_arbiter(4,{1,1,1}), other=make_execution_arbiter(8,{1,1,1});
    rejects([&]{unit->arbitrate(port(0,7),0,false);},"execution_arbiter_invalid_requests");
    rejects([&]{unit->arbitrate(0,1,false);},"execution_arbiter_invalid_requests");
    rejects([&]{unit->arbitrate(uint64_t{1}<<63,0,false);},"execution_arbiter_request_width");
    require(unit->arbitrate(port(0,0)|port(5,3),0,false)==(port(0,0)|port(5,3)),"bad request corrupted following batch");
    unit->accept(0,0);
    rejects([&]{unit->accept(0,0);},"execution_arbiter_not_granted");
    rejects([&]{unit->arbitrate(0,0,false);},"execution_arbiter_already_arbitrated");
    rejects([&]{unit->accept(5,2);},"execution_arbiter_not_granted");
    unit->accept(5,3); unit->tick();
    require(unit->read().tensor_owner==3 && !other->read().tensor_busy && !other->stats().clocks,"instances shared state");
}
void run_cases() {
    clocks_snapshots_and_geometries(); std::cout<<"PASS arbiter adapter clocks, snapshots and 1/2/4/8 partitions\n";
    tensor_lease_and_same_cycle_handoff(); std::cout<<"PASS arbiter adapter Tensor lease and retirement handoff\n";
    downstream_refusal_async_and_intervals(); std::cout<<"PASS arbiter adapter downstream refusal, async no-op and intervals\n";
    invalid_events_phases_and_isolation(); std::cout<<"PASS arbiter adapter invalid events, phases and isolation\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

#include "simt/command_processor.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool caught=false; try { action(); } catch (const std::exception& e) { caught=e.what()==expected; }
    require(caught,"wrong or missing command processor exception");
}

// Reads and pin changes never advance the circuit; only tick() creates an edge.
// 读取和引脚变化不会推进电路；只有 tick() 产生边沿。
void reads_do_not_create_edges() {
    auto cp=make_command_processor(8);
    require(!cp->published() && !cp->fetched() && !cp->notified() && !cp->stats().clocks,"reset state wrong");
    cp->ring_doorbell(2); require(cp->published()==2 && !cp->stats().clocks,"doorbell used a hidden edge");
    for (uint32_t n=0; n<20; ++n) require(cp->published()==2 && !cp->fetched(),"read changed held doorbell");
    const auto slot=cp->fetch_slot(); require(slot && *slot==0,"published packet not fetchable");
    cp->accept_fetch(7,EngineKind::compute,0); require(!cp->fetched(),"fetch committed before its edge");
    cp->tick(); require(cp->fetched()==1 && cp->published()==2 && cp->stats().clocks==1,"edge did not commit");
    require(cp->lookup(7)==1 && !cp->lookup(8),"slot lookup wrong");
}

void dispatch_complete_and_release_cycle() {
    auto cp=make_command_processor(8); cp->ring_doorbell(1);
    cp->accept_fetch(1,EngineKind::compute,0);
    const auto chosen=cp->dispatch(); require(chosen && !chosen->node && !chosen->cancel && !chosen->slot,"packet not selected");
    cp->tick(); require(cp->busy(EngineKind::compute) && !cp->dispatch(),"engine not occupied");
    cp->complete(0,false); cp->release(EngineKind::compute); require(!cp->notified(),"completion before edge"); cp->tick();
    require(cp->notified()==1 && !cp->busy(EngineKind::compute),"completion not committed");
    const auto& s=cp->stats(); require(s.fetched==1 && s.dispatched==1 && s.completed==1 && !s.failed && s.peak_slots==1,"event counts wrong");
}

void older_node_wins_and_start_failure_returns_engine() {
    auto cp=make_command_processor(8); cp->ring_doorbell(1);
    cp->accept_fetch(50,EngineKind::copy,0);
    cp->offer_node(CpNode{40,EngineKind::compute,false});
    auto chosen=cp->dispatch(); require(chosen && chosen->node,"older node lost"); cp->release(EngineKind::compute); cp->tick();
    require(!cp->busy(EngineKind::compute),"failed start kept engine");
    cp->offer_node(CpNode{60,EngineKind::compute,false});
    chosen=cp->dispatch(); require(chosen && !chosen->node,"younger node beat packet"); cp->offer_node({}); cp->tick();
    require(cp->busy(EngineKind::copy),"packet did not claim copy engine");
}

void protocol_errors_are_reported() {
    auto cp=make_command_processor(2);
    rejects([&]{cp->ring_doorbell(3);},"command_processor_protocol_error 2");
    cp->ring_doorbell(0);
    rejects([&]{cp->accept_fetch(1,EngineKind::compute,0);},"command_processor_protocol_error 3");
    rejects([&]{make_command_processor(0);},"invalid_command_processor_capacity");
    rejects([&]{make_command_processor(4097);},"invalid_command_processor_capacity");
}
void run_cases() {
    reads_do_not_create_edges(); std::cout<<"PASS CP adapter reads and pins create no hidden edges\n";
    dispatch_complete_and_release_cycle(); std::cout<<"PASS CP adapter dispatch, completion and engine release\n";
    older_node_wins_and_start_failure_returns_engine(); std::cout<<"PASS CP adapter node arbitration and failed start\n";
    protocol_errors_are_reported(); std::cout<<"PASS CP adapter protocol errors\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

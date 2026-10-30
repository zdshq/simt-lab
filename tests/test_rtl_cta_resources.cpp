#include "simt/cta_resources.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool caught=false; try { action(); } catch (const std::exception& e) { caught=e.what()==expected; }
    require(caught,"wrong or missing resource adapter exception");
}
CtaResourceInputs exited() {
    CtaResourceInputs inputs; for (auto& warp : inputs.states) warp.fill(5); return inputs;
}
void explicit_edges_and_partial_warps() {
    auto unit=make_cta_resources({}); auto inputs=exited(); unit->observe(inputs);
    const auto grant=unit->acquire(UINT32_MAX,33,129);
    require(grant && grant->slot==0 && grant->residency.slots.size()==2 && grant->residency.shared==256,"grant incorrect");
    for (uint32_t i=0; i<20; ++i) require(!unit->query(UINT32_MAX) && !unit->stats().clocks,"query advanced a hidden clock");
    require(!unit->acquire(2,1,0),"single input port accepted two requests"); unit->tick();
    inputs.states[1][31]=0; unit->observe(inputs); require(unit->query(UINT32_MAX)->drained,"unused lane blocked retirement");
    inputs.inflight[1]=UINT32_MAX; unit->observe(inputs); require(!unit->query(UINT32_MAX)->drained,"inflight count narrowed");
    rejects([&]{unit->begin_release(UINT32_MAX);},"cta_resource_event_error");
    inputs.inflight[1]=0; inputs.states[1][0]=4; unit->observe(inputs);
    require(!unit->query(UINT32_MAX)->drained,"active partial warp ignored");
    unit->observe(exited()); unit->begin_release(UINT32_MAX); unit->tick(); unit->release(UINT32_MAX); unit->tick();
    const auto active=unit->stats().active_clocks; unit->tick();
    require(!unit->read().live && unit->stats().admitted==1 && unit->stats().released==1 && active==unit->stats().active_clocks,"credits or gating wrong");
}

// A newly initialized frontend may replace old lane wires after the release was qualified and latched.
// 释放资格已锁存后，新初始化的前端可以替换旧 lane 连线。
void held_grants_and_retirement_handoff() {
    auto unit=make_cta_resources({}); unit->observe(exited());
    for (uint32_t id=0; id<4; ++id) { require(bool(unit->acquire(id,32,128)),"admission failed"); unit->tick(); }
    unit->begin_release(0); unit->begin_release(3); unit->tick(); unit->release(3);
    const auto grant=unit->acquire(77,32,128); require(grant && grant->slot==3,"wrong reuse slot");
    unit->release(0); auto inputs=exited(); inputs.states[3].fill(0); unit->observe(inputs); unit->tick();
    require(unit->query(77)->slot==3 && unit->query(77)->warps==8 && !unit->query(0),"late release retargeted grant");
    require(!unit->query(77)->drained,"new owner's active lanes ignored"); unit->observe(exited());
    for (auto id : {1u,2u,77u}) unit->begin_release(id);
    unit->tick(); for (auto id : {1u,2u,77u}) unit->release(id);
    unit->tick(); require(unit->stats().admitted==unit->stats().released,"handoff credits leaked");
}
void dma_table_and_invalid_events() {
    auto unit=make_cta_resources({}); unit->observe(exited()); unit->acquire(9,1,0); unit->tick();
    unit->initialize_table(9); unit->start_dma(9);
    rejects([&]{unit->start_dma(9);},"duplicate_cta_resource_event"); unit->tick();
    require(unit->query(9)->dma==1 && !unit->query(9)->drained,"DMA not counted");
    rejects([&]{unit->clean_table(9);},"cta_resource_event_error"); unit->finish_dma(9); unit->tick();
    require(unit->read().cleanup==1,"cleanup not requested");
    rejects([&]{unit->begin_release(9);},"cta_resource_event_error"); unit->clean_table(9); unit->tick();
    unit->begin_release(9); unit->tick(); unit->release(9); unit->tick();
    require(unit->stats().table_cleanups==1 && unit->stats().dma_started==unit->stats().dma_finished,"lifecycle counters wrong");
    rejects([&]{unit->release(9);},"unknown_resident_cta");
}
void full_capacity_validation_and_isolation() {
    PipelineConfig config; config.max_ctas=64;
    auto unit=make_cta_resources(config), other=make_cta_resources(config); unit->observe(exited());
    for (uint32_t id=0; id<32; ++id) { require(bool(unit->acquire(id,32,0)),"full capacity unavailable"); unit->tick(); }
    require(!unit->acquire(33,1,0) && unit->read().registers==65536,"32 warp capacity wrong");
    rejects([&]{unit->acquire(0,1,0);},"invalid_cta_resource_request");
    auto invalid=exited(); invalid.states[31][31]=6;
    rejects([&]{unit->observe(invalid);},"invalid_cta_resource_lane_state");
    for (uint32_t id=0; id<32; ++id) unit->begin_release(id);
    unit->tick(); for (uint32_t id=0; id<32; ++id) unit->release(id);
    require(bool(unit->acquire(99,1024,65536)),"same-edge full handoff failed"); unit->tick();
    require(unit->query(99)->shared==65536 && !other->read().live && !other->stats().clocks,"full width or isolation failed");
    config.max_ctas=65; rejects([&]{make_cta_resources(config);},"invalid_cta_resource_rtl_configuration");
}
void run_cases() {
    explicit_edges_and_partial_warps(); std::cout<<"PASS resource adapter explicit edges, partial lanes and inflight wires\n";
    held_grants_and_retirement_handoff(); std::cout<<"PASS resource adapter held grants and new frontend handoff\n";
    dma_table_and_invalid_events(); std::cout<<"PASS resource adapter DMA/table lifecycle and event rollback\n";
    full_capacity_validation_and_isolation(); std::cout<<"PASS resource adapter full capacity, validation and SM isolation\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

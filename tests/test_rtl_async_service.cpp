#include "simt/async_service.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool caught=false; try { action(); } catch (const std::exception& e) { caught=e.what()==expected; }
    require(caught,"wrong or missing async service exception");
}
void clocks_delays_and_snapshots() {
    auto unit=make_async_service(64,3); unit->accept_instruction(UINT64_MAX,UINT32_MAX);
    require(!unit->stats().clocks && !unit->read().ready,"accept advanced a hidden clock"); unit->tick();
    for (uint32_t query=0; query<20; ++query) {
        auto state=unit->read(); state.id=0;
        require(unit->read().id!=state.id && unit->read().remaining==2,"query changed hardware");
    }
    unit->tick(); unit->tick(); require(unit->read().ready,"latency deadline missed");
    unit->retry(); require(!unit->read().ready,"retry failed to withdraw ready");
    unit->tick(); unit->tick(); unit->tick(); require(unit->read().ready,"retry deadline missed");
    unit->complete(); unit->tick(); const auto clocks=unit->stats().active_clocks; unit->tick();
    require(unit->stats().active_clocks==clocks && unit->stats().retries==1,"idle clock or retry event wrong");
    require(unit->stats().accepted[0]==1 && unit->stats().completed[0]==1,"foreground counts wrong");
}
void offered_wires_and_handoff() {
    auto unit=make_async_service(64,1); AsyncServiceInputs inputs;
    inputs.table_ready=true; inputs.retire_requests=uint64_t{1}<<63; inputs.retire_ctas[63]=UINT32_MAX;
    unit->offer(inputs); unit->accept_background(); unit->offer({});
    require(unit->read().kind==AsyncOwner::retirement && unit->read().cta==UINT32_MAX,"accepted wires changed");
    unit->tick(); require(unit->read().slot==63 && unit->read().cursor==0,"highest physical slot truncated");
    unit->complete(); unit->tick(); unit->tick(); require(unit->read().retirement_done,"retirement not retained");
    unit->acknowledge_retirement(); inputs.dma_valid=true; inputs.dma_id=UINT64_MAX; inputs.dma_cta=7;
    unit->offer(inputs); unit->accept_background(); unit->tick();
    require(unit->read().kind==AsyncOwner::dma && unit->read().id==UINT64_MAX,"retirement handoff lost DMA");
    unit->complete(); unit->accept_instruction(11,9); unit->tick();
    require(unit->read().id==11 && unit->stats().completed[1]==1,"DMA to foreground handoff failed");
    unit->complete(); unit->tick();
    require(unit->stats().retirement_acks==1 && unit->stats().completed==unit->stats().accepted,"events unbalanced");
}
void quotas_errors_and_isolation() {
    for (uint32_t ctas : {1u,2u,32u,63u,64u}) {
        auto unit=make_async_service(ctas,1); AsyncServiceInputs inputs;
        inputs.retire_requests=uint64_t{1}<<(ctas-1); unit->offer(inputs);
        require(!unit->foreground_ready() && unit->read().pending,"pending release lost under backpressure");
        inputs.table_ready=true; unit->offer(inputs); require(unit->selection().slot==ctas-1,"quota slot not selectable");
        if (ctas<64) {
            inputs.retire_requests=uint64_t{1}<<ctas;
            rejects([&]{unit->offer(inputs);},"async_service_retirement_mask");
            require(unit->selection().slot==ctas-1,"bad offer mutated wires");
        }
    }
    rejects([]{make_async_service(65,1);},"invalid_async_service_configuration");
    rejects([]{make_async_service(0,1);},"invalid_async_service_configuration");
    rejects([]{make_async_service(1,0);},"invalid_async_service_configuration");
    auto unit=make_async_service(1,2), other=make_async_service(64,1);
    rejects([&]{unit->complete();},"async_service_completion_phase");
    unit->accept_instruction(19,42); rejects([&]{unit->complete();},"async_service_completion_phase"); unit->tick();
    rejects([&]{unit->complete();},"async_service_completion_phase"); unit->tick();
    require(unit->read().ready && other->read().kind==AsyncOwner::idle && !other->stats().clocks,"rollback or isolation failed");
    unit->complete(); rejects([&]{unit->complete();},"async_service_completion_phase"); unit->tick();
    require(unit->read().kind==AsyncOwner::idle,"valid completion lost after rejection");
}
void run_cases() {
    clocks_delays_and_snapshots(); std::cout<<"PASS async adapter explicit clocks, snapshots and retry\n";
    offered_wires_and_handoff(); std::cout<<"PASS async adapter accepted wires, full-width ownership and handoff\n";
    quotas_errors_and_isolation(); std::cout<<"PASS async adapter quotas, rollback and instance isolation\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

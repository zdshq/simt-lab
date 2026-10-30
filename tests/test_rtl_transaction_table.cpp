#include "simt/transaction_table.hpp"
#include "simt/warp_frontend.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool failed=false; try { action(); } catch (const std::exception& e) { failed=e.what()==expected; }
    require(failed,"wrong or missing transaction exception");
}
TransactionResult execute(TransactionTable& unit,TransactionCommand command) {
    unit.submit(command);
    for (uint32_t i=0; i<6000; ++i) {
        unit.tick();
        if (unit.publication()) unit.acknowledge();
        if (auto response=unit.take()) { unit.tick(); return *response; }
    }
    throw std::runtime_error("transaction adapter timeout");
}
void publication_backpressure_and_observation() {
    auto unit=make_transaction_table(16); unit->submit({TransactionOp::initialize,0,1024,1});
    rejects([&]{unit->submit({TransactionOp::query,0,1024});},"transaction_command_phase");
    for (uint32_t i=0; i<100 && !unit->publication(); ++i) unit->tick();
    require(bool(unit->publication()),"publication missing"); const auto clocks=unit->stats().clocks;
    for (uint32_t i=0; i<30; ++i) {
        auto record=*unit->publication(); record.record.phase=999;
        require(unit->publication()->record.phase!=record.record.phase,"snapshot fed back into storage");
        require(!unit->take() && !unit->occupancy(),"early publication");
    }
    require(unit->stats().clocks==clocks && unit->publication()->record.phase==0,"observation mutated table or clock");
    for (uint32_t i=0; i<12; ++i) unit->tick();
    unit->acknowledge(); rejects([&]{unit->acknowledge();},"transaction_publication_phase"); unit->tick();
    const auto result=unit->take(); require(result && !result->error && result->record.expected==1,"bad init response");
    require(!unit->take() && !unit->available(),"response consumed twice or early new command"); unit->tick();
    require(unit->stats().commands==1 && unit->stats().publications==1 && unit->stats().responses==1,"handshake accounting");
}
void independent_instances_capacity_and_reuse() {
    rejects([]{make_transaction_table(0);},"invalid_transaction_capacity");
    rejects([]{make_transaction_table(1025);},"invalid_transaction_capacity");
    auto a=make_transaction_table(1),b=make_transaction_table(1024);
    require(!execute(*a,{TransactionOp::initialize,0,0,1}).error,"init a");
    require(!execute(*b,{TransactionOp::initialize,0,0,1024}).error,"init b");
    require(execute(*a,{TransactionOp::initialize,1,0,1}).error==5,"quota ignored");
    require(execute(*a,{TransactionOp::arrive,0,0}).record.phase==1,"phase a");
    require(execute(*b,{TransactionOp::query,0,0}).record.phase==0,"instances share state");
    require(!execute(*a,{TransactionOp::release,0}).error && !a->occupancy(),"slot not freed");
    require(execute(*a,{TransactionOp::initialize,UINT32_MAX,65504,37}).record.arrivals==37,"reused slot inherited state");
}
void rejected_release_retains_all_records() {
    auto unit=make_transaction_table(16);
    execute(*unit,{TransactionOp::initialize,0,0,1}); execute(*unit,{TransactionOp::initialize,0,32,1});
    execute(*unit,{TransactionOp::expect,0,32,4}); execute(*unit,{TransactionOp::arrive,0,32});
    execute(*unit,{TransactionOp::issue,0,32,4});
    require(execute(*unit,{TransactionOp::release,0}).error==14 && unit->occupancy()==2,"failed release partly freed entries");
    require(!execute(*unit,{TransactionOp::query,0,0}).error,"clean predecessor lost");
    require(execute(*unit,{TransactionOp::complete,0,32,4,1}).error==11,"wrong completion phase accepted");
    require(execute(*unit,{TransactionOp::query,0,32}).record.issued==4,"rejection changed byte accounting");
    require(execute(*unit,{TransactionOp::complete,0,32,4,0}).advanced,"completion did not advance phase");
    require(!execute(*unit,{TransactionOp::release,0}).error && !unit->occupancy(),"complete CTA not released");
}

// A consumer stays asleep throughout a pending shared publication, then uses the committed raw phase bus.
// 消费者在 shared 发布等待期间保持休眠，随后使用已提交的原始阶段总线。
void publication_to_frontend_without_host_comparison() {
    auto table=make_transaction_table(16); auto warp=make_warp_frontend(); warp->initialize(1); warp->tick();
    execute(*table,{TransactionOp::initialize,0,1024,1});
    const auto query=execute(*table,{TransactionOp::query,0,1024}); std::array<uint32_t,32> phases{};
    warp->async_wait(1,1024,query.record.phase,phases); warp->tick(); require(!warp->select(0),"wait not stored");
    table->submit({TransactionOp::arrive,0,1024});
    for (uint32_t i=0; i<100 && !table->publication(); ++i) table->tick();
    const auto pending=table->publication();
    require(pending && pending->advanced,"arrival did not prepare next phase");
    for (uint32_t i=0; i<17; ++i) { table->tick(); warp->tick(); require(!warp->select(0),"wake before shared ack"); }
    table->acknowledge(); table->tick(); const auto complete=table->take(); require(bool(complete),"published response missing");
    warp->async_release(complete->address,complete->record.phase);
    auto group=warp->select(0); require(group && group->mask==1,"raw phase bus did not wake consumer");
    warp->issue(*group,true); warp->tick(); table->tick(); require(warp->read().control.lanes.pc[0]==1,"wake-to-issue lost");
}
void run_cases() {
    publication_backpressure_and_observation(); std::cout<<"PASS transaction adapter finite port, publication ack, clocks and snapshot isolation\n";
    independent_instances_capacity_and_reuse(); std::cout<<"PASS transaction adapter independent instances, quotas and slot reuse\n";
    rejected_release_retains_all_records(); std::cout<<"PASS transaction adapter atomic retirement rejection and byte/phase errors\n";
    publication_to_frontend_without_host_comparison(); std::cout<<"PASS transaction publication-to-frontend phase bus and delayed wakeup\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

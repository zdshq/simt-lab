#include "simt/cta_barrier.hpp"
#include "simt/warp_frontend.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool failed=false; try { action(); } catch (const std::exception& e) { failed=e.what()==expected; }
    require(failed,"missing or incorrect CTA adapter error");
}

// One pending command is held until the shared edge; snapshots and availability checks never advance clocks.
// 单个待提交命令保持到共享边沿，快照与可用性查询不推进时钟。
void single_port_clocks_and_atomic_errors() {
    auto table=make_cta_barrier(); require(!table->available(),"uninitialized table accepted commands");
    rejects([&]{table->initialize(1025);},"invalid_cta_threads");
    table->initialize(64); rejects([&]{table->execute({false,0,1,0,0});},"cta_barrier_command_phase"); table->tick();
    require(table->execute({false,0,UINT32_MAX,15,64})==0,"premature explicit completion");
    rejects([&]{table->execute({true,0,1,0,0});},"cta_barrier_command_phase");
    rejects([&]{table->initialize(32);},"cta_barrier_initialize_phase");
    auto copy=table->read(); copy.records[15].members.fill(0); copy.live_count=0;
    require(table->stats().clocks==1 && table->stats().commands==0,"submission advanced clocks"); table->tick();
    rejects([&]{table->execute({false,0,3,15,64});},"duplicate_barrier_arrival");
    rejects([&]{table->execute({false,1,1,15,32});},"barrier_count_mismatch");
    rejects([&]{table->execute({false,32,1,0,0});},"cta_barrier_descriptor");
    for (uint32_t i=0; i<20; ++i) require(table->available() && table->read().records[15].arrived==32,"rejection or query corrupted storage");
    require(table->execute({false,1,UINT32_MAX,15,64})==(1u<<15),"pending arrival was lost"); table->tick();
    require(table->stats().commands==2 && table->stats().completions==1 && table->stats().clocks==3,"event accounting");
    table->tick(); require(table->stats().active_clocks==3,"idle table clock enabled");
}

// Physical CTA instances remain isolated, including the last warp and all sixteen simultaneous generations.
// 物理 CTA 实例相互隔离，包括最后一个 warp 和十六个同时存在的阶段。
void independent_ctas_full_capacity_and_reuse() {
    auto first=make_cta_barrier(),second=make_cta_barrier();
    first->initialize(1024); first->tick(); second->initialize(37); second->tick();
    for (uint32_t id=0; id<16; ++id) { first->execute({false,31,0x80000000,id,0}); first->tick(); }
    auto state=first->read();
    for (const auto& record : state.records) require(record.active && record.members[31]==0x80000000 && record.expected==1024,"last warp/record storage");
    require(second->read().records[15].arrived==0 && second->read().live_count==37,"CTA slots aliased");
    first->execute({true,31,0x80000000,0,0}); first->tick();
    for (uint32_t warp=0; warp<32; ++warp) { first->execute({true,warp,UINT32_MAX,0,0}); first->tick(); }
    require(first->stats().completions==16 && first->read().live_count==0,"dynamic EXIT accounting");
    first->initialize(37); first->tick();
    first->execute({false,0,UINT32_MAX,15,0}); first->tick();
    require(first->execute({false,1,31,15,0})==(1u<<15),"tail warp or reuse failed"); first->tick();
    require(first->read().records[15].generation==1 && first->stats().initializations==2,"slot clear or cumulative accounting");
}

// Route the exact table bus into two frontend DUTs and consume the woken group before the same clock edge.
// 将表的原始总线送入两个前端 DUT，并在同一边沿之前消费已唤醒线程组。
void table_to_frontend_wait_and_release() {
    auto table=make_cta_barrier(); auto first=make_warp_frontend(),second=make_warp_frontend();
    table->initialize(64); first->initialize(UINT32_MAX); second->initialize(UINT32_MAX);
    table->tick(); first->tick(); second->tick();
    auto release=table->execute({false,0,UINT32_MAX,15,0});
    first->advance({4,UINT32_MAX}); first->cta_barrier(UINT32_MAX,15,release);
    require(!first->select(0),"waiting warp remained runnable"); table->tick(); first->tick(); second->tick();
    release=table->execute({false,1,UINT32_MAX,15,0});
    second->advance({9,UINT32_MAX}); second->cta_barrier(UINT32_MAX,15,release); first->cta_barrier(0,0,release);
    auto a=first->select(0),b=second->select(0);
    require(a && b && a->pc==5 && b->pc==10,"table release lost per-warp continuation");
    first->issue(*a,true); second->issue(*b,true); table->tick(); first->tick(); second->tick();
    require(first->read().control.lanes.pc[31]==6 && second->read().control.lanes.pc[31]==11,"release-to-issue did not persist");
    require(first->frontend_stats().cta_woken==32 && second->frontend_stats().cta_woken==32,"cross-warp wake accounting");
}

// Dynamic target shrink comes from table-owned live bits, even when the exiting peer never called BAR_SYNC.
// 动态目标缩减来自表持有的存活位，即使退出同伴从未调用 BAR_SYNC。
void exit_releases_peer_and_rejected_overflow() {
    auto table=make_cta_barrier(); auto waiting=make_warp_frontend();
    table->initialize(64); waiting->initialize(UINT32_MAX); table->tick(); waiting->tick();
    table->execute({false,0,UINT32_MAX,7,0}); waiting->advance({11,UINT32_MAX}); waiting->cta_barrier(UINT32_MAX,7,0);
    table->tick(); waiting->tick();
    const auto release=table->execute({true,1,UINT32_MAX,0,0}); waiting->cta_barrier(0,0,release);
    const auto selected=waiting->select(0);
    require(release==(1u<<7) && selected && selected->pc==12,"EXIT failed to wake peer"); table->tick(); waiting->tick();
    table->initialize(32); table->tick();
    rejects([&]{table->execute({false,0,UINT32_MAX,0,16});},"barrier_arrival_overflow");
    require(!table->read().records[0].active && table->read().records[0].arrived==0,"overflow partially modified record");
    require(table->execute({false,0,0xffff,0,16})==1,"valid arrival after overflow rejected"); table->tick();
}
void run_cases() {
    single_port_clocks_and_atomic_errors(); std::cout<<"PASS CTA adapter finite port, clocks, atomic errors and snapshot isolation\n";
    independent_ctas_full_capacity_and_reuse(); std::cout<<"PASS CTA adapter independent slots, 1024 threads, sixteen records and reuse\n";
    table_to_frontend_wait_and_release(); std::cout<<"PASS CTA table-to-frontend bus, cross-warp wait and same-cycle wake-to-issue\n";
    exit_releases_peer_and_rejected_overflow(); std::cout<<"PASS CTA EXIT peer wakeup and overflow rollback\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

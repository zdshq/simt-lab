#include "simt/warp_frontend.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view message) {
    bool failed=false; try { action(); } catch (const std::exception& e) { failed=e.what()==message; }
    require(failed,"missing or incorrect frontend error");
}
FrontendWait waits(const FrontendState& s) { return {s.control.lanes.state,s.control.lanes.blocked,s.phase}; }
ControlCommand branch(uint32_t pc,uint32_t group,uint32_t mask,uint32_t target) { return {25,0,7,pc,group,mask,target,128,false}; }

// A changed observation cannot alter stored PCs, grouping, or the cursor after the next edge.
// 修改观察快照不能改变下一边沿之后的已存 PC、分组或游标。
void persistent_pc_grouping_and_read_isolation() {
    auto unit=make_warp_frontend(); unit->initialize(0x1ffff);
    auto group=unit->select(0); require(group && group->pc==0 && group->mask==0x1ffff,"first-cycle group");
    unit->issue(*group,true); require(unit->stats().clocks==0,"issue inserted hidden clock"); unit->tick();
    auto snapshot=unit->execute(branch(1,0x1ffff,0xffff,20));
    snapshot.control.lanes.pc.fill(999); snapshot.last_pc=999; snapshot.control.tokens[15]={1,1,1,999};
    unit->tick(); group=unit->select(1);
    require(group && group->pc==2 && group->mask==0x10000,"host snapshot replaced PC storage");
    unit->issue(*group,false); unit->tick(); group=unit->select(0);
    require(group && group->pc==20 && group->mask==0xffff,"round-robin successor cursor");
    for (uint32_t i=0; i<20; ++i) { auto value=unit->read(); require(value.control.lanes.pc[0]==20,"read changed storage"); }
    require(unit->stats().clocks==3 && unit->frontend_stats().issued==2,"queries changed accounting");
}

// Pre/control/post/issue ordering must preserve full addresses and the states crossing packed word boundaries.
// 控制前/控制/控制后/发射顺序必须保留完整地址和跨打包字边界的状态。
void ordered_wait_writes_and_same_cycle_issue() {
    auto unit=make_warp_frontend(); unit->initialize(UINT32_MAX); unit->tick();
    auto external=waits(unit->read());
    for (uint32_t lane : {10,21,31}) { external.state[lane]=4; external.blocked[lane]=0xfedcba98+lane; external.phase[lane]=0xffff0000+lane; }
    unit->external(external);
    unit->execute(branch(0,0x7fdffbff,0xffff & 0x7fdffbff,8));
    external=waits(unit->read()); external.state[31]=0; unit->external(external);
    const auto group=unit->select(2); require(group && group->pc==8,"branch-to-selector bypass");
    unit->issue(*group,true); unit->tick(); const auto stored=unit->read();
    require(stored.control.lanes.state[10]==4 && stored.control.lanes.state[21]==4 && stored.control.lanes.state[31]==0,"pre/post state ownership");
    for (uint32_t lane : {10,21,31}) require(stored.control.lanes.blocked[lane]==0xfedcba98+lane && stored.phase[lane]==0xffff0000+lane,"full-width wait metadata");
    require(stored.control.lanes.pc[0]==9 && stored.control.lanes.pc[31]==0,"issue masked stored PC");
    require(unit->frontend_stats().external_lanes==4,"ordered external write counts");
}

// Hardware stores yielded lanes until a selection opportunity or successful peer issue resumes them.
// 硬件保存已让出的 lane，直到选择机会或同伴成功发射使它们恢复。
void yielded_lane_lifetime_and_slot_reuse() {
    auto unit=make_warp_frontend(); unit->initialize(UINT32_MAX); unit->tick();
    unit->execute({29,0,7,0,UINT32_MAX,0xffff,0,128,false}); unit->tick();
    auto group=unit->select(0); require(group && group->mask==0xffff0000,"yielded lanes joined active peers early");
    require(unit->read().control.lanes.state[0]==2,"probe resumed yielded peers with active lanes");
    unit->issue(*group,false); unit->tick(); require(unit->read().control.lanes.state[0]==0,"successful issue did not resume peers");
    unit->execute({29,0,7,1,UINT32_MAX,UINT32_MAX,0,128,false}); unit->tick();
    group=unit->select(0); require(group && group->pc==2,"idle yielded group not selected");
    unit->tick(); require(unit->read().control.lanes.state[31]==0,"idle resume lost when issue stalled");
    require(unit->frontend_stats().resumed_lanes==48,"yield resume accounting");
    unit->initialize(1); group=unit->select(0); require(group && group->pc==0 && group->mask==1,"slot reuse selection");
    unit->issue(*group,true); unit->tick(); require(!unit->read().control.tokens[15].state,"slot reuse leaked token");
}

// Rejected calls leave previously submitted transactions and stored token ownership intact.
// 被拒绝的调用保留先前提交的事务和已存令牌归属。
void phase_guards_and_rejected_transactions() {
    auto unit=make_warp_frontend(); unit->initialize(UINT32_MAX); unit->tick();
    ControlCommand arm{26,15,7,0,UINT32_MAX,UINT32_MAX,64,128,true}; unit->execute(arm);
    rejects([&]{unit->advance({0,1});},"frontend_advance_phase");
    rejects([&]{unit->execute(arm);},"frontend_control_phase");
    rejects([&]{unit->initialize(1);},"frontend_initialize_phase"); unit->tick();
    rejects([&]{unit->execute(arm);},"live_barrier_overwrite");
    rejects([&]{unit->select(3);},"frontend_policy");
    auto bad=waits(unit->read()); bad.state[31]=6;
    rejects([&]{unit->external(bad);},"control_lane_state");
    auto group=unit->select(0); require(group.has_value(),"state lost on error");
    rejects([&]{unit->issue({group->pc,1},true);},"frontend_issue_group_mismatch");
    unit->issue(*group,true);
    rejects([&]{unit->select(0);},"frontend_select_after_issue");
    rejects([&]{unit->issue(*group,true);},"frontend_duplicate_issue");
    rejects([&]{unit->external(waits(unit->read()));},"frontend_external_phase"); unit->tick();
    require(unit->read().control.tokens[15].members==UINT32_MAX && unit->stats().commands==1,"errors lost token or counted command");
}

// Raw CTA signals change only matching local waiters and obey the same single-edge lifecycle as issue.
// 原始 CTA 信号只改变匹配的本地等待者，并遵循与发射相同的单边沿生命周期。
void cta_signal_routing_and_async_isolation() {
    auto unit=make_warp_frontend(); unit->initialize(UINT32_MAX); unit->tick();
    rejects([&]{unit->cta_barrier(1,16,0);},"frontend_cta_descriptor");
    rejects([&]{unit->cta_barrier(0,0,65536);},"frontend_cta_descriptor");
    unit->advance({3,UINT32_MAX}); unit->cta_barrier(0xff,15,0); unit->tick();
    auto external=waits(unit->read()); external.state[8]=4; external.blocked[8]=15; external.phase[8]=23;
    external.state[9]=3; external.blocked[9]=0x1000f; unit->external(external);
    unit->cta_barrier(0,0,1u<<15);
    auto group=unit->select(0); require(group && group->mask==(UINT32_MAX^0x300),"CTA adapter woke wrong lanes");
    unit->issue(*group,true);
    rejects([&]{unit->cta_barrier(0,0,1);},"frontend_cta_phase"); unit->tick();
    const auto stored=unit->read();
    require(stored.control.lanes.pc[0]==5 && stored.control.lanes.state[8]==4 && stored.control.lanes.state[9]==3,"CTA/async state isolation");
    require(unit->frontend_stats().cta_blocked==8 && unit->frontend_stats().cta_woken==8,"CTA event accounting");
}
void asynchronous_query_rejection_and_publication() {
    auto unit=make_warp_frontend(); unit->initialize(UINT32_MAX); unit->tick();
    std::array<uint32_t,32> phases{}; phases[0]=3; phases[31]=4;
    rejects([&]{unit->async_wait(0x80000001,0xffff0000,3,phases);},"mbarrier_future_phase");
    require(unit->read().control.lanes.state[0]==0,"future phase partially blocked other lanes");
    phases[31]=3; unit->async_wait(0x80000001,0xffff0000,3,phases);
    rejects([&]{unit->async_wait(1,0,0,phases);},"frontend_async_wait_phase"); unit->tick();
    unit->async_release(0x00000000,4); unit->tick();
    require(unit->read().control.lanes.state[31]==4,"address comparison truncated");
    unit->async_release(0xffff0000,4); auto group=unit->select(0);
    require(group && group->mask==UINT32_MAX,"new phase did not wake stored waiters"); unit->issue(*group,true);
    rejects([&]{unit->async_release(0,1);},"frontend_async_release_phase"); unit->tick();
    require(unit->frontend_stats().async_checked==2 && unit->frontend_stats().async_blocked==2 &&
        unit->frontend_stats().async_woken==2,"async adapter event accounting");
}
void run_cases() {
    persistent_pc_grouping_and_read_isolation(); std::cout<<"PASS frontend adapter stored PCs, selection cursor and observation isolation\n";
    ordered_wait_writes_and_same_cycle_issue(); std::cout<<"PASS frontend adapter pre/control/post/issue ordering and packed metadata\n";
    yielded_lane_lifetime_and_slot_reuse(); std::cout<<"PASS frontend adapter yielded lifetime, stalled resume and physical slot reuse\n";
    phase_guards_and_rejected_transactions(); std::cout<<"PASS frontend adapter phase guards, rejected writes and preserved token ownership\n";
    cta_signal_routing_and_async_isolation(); std::cout<<"PASS frontend adapter CTA signal routing, wakeup and asynchronous isolation\n";
    asynchronous_query_rejection_and_publication(); std::cout<<"PASS frontend adapter async phase rejection, full addresses and completion wakeup\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

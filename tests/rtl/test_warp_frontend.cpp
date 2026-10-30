#include "bench.hpp"
#include "Vwarp_frontend.h"
#include "simt/control.hpp"
#include "simt/warp_control.hpp"
#include <map>
#include <random>

namespace {
using TestBench = Bench<Vwarp_frontend>;
using simt::LaneState;
using simt::hw::ControlCommand;

// Withdraw cycle transactions while leaving all stored state untouched.
// 撤销当前拍事务，不改变任何持久状态。
void pins(TestBench& b) {
    auto& d=b.dut; d.initialize=0; d.command_valid=0; d.retire_advance=0; d.resume_idle=0; d.issue_valid=0;
    d.write_state_mask=0; d.write_blocked_mask=0; d.write_phase_mask=0; d.eval();
    d.cta_block_mask=0; d.cta_release_mask=0; d.eval();
}
void check(TestBench& b, const simt::Warp& w) {
    const auto& d=b.dut;
    for (uint32_t lane=0; lane<32; ++lane) {
        const auto shift=lane*3; uint64_t bits=d.observed_state[shift/32];
        if (shift%32>29) bits|=uint64_t{d.observed_state[shift/32+1]}<<32;
        require(d.observed_pc[lane]==w.pc[lane] && ((bits>>(shift%32))&7)==static_cast<uint32_t>(w.state[lane]),"stored PC/state differs");
        require(d.observed_blocked[lane]==w.blocked_token[lane] && d.observed_phase[lane]==w.waiting_phase[lane],"stored wait metadata differs");
    }
    require(d.observed_valid_mask==w.valid_mask && bool(d.observed_last_valid)==w.last_pc.has_value(),"valid mask/cursor validity differs");
    if (w.last_pc) require(d.observed_last_pc==*w.last_pc,"round-robin cursor differs");
    for (uint32_t id=0; id<16; ++id) {
        const auto& t=w.reconvergence[id];
        require(((d.token_states>>(id*2))&3)==static_cast<uint32_t>(t.state) && d.token_members[id]==t.participants &&
                d.token_arrivals[id]==t.arrived && d.token_targets[id]==t.target_pc,"connected token storage differs");
    }
}
void tick(TestBench& b, const simt::Warp& w) { b.dut.eval(); check(b,w); b.tick(); pins(b); check(b,w); }
void initialize(TestBench& b, simt::Warp& w, uint32_t mask) {
    pins(b); b.dut.initialize=1; b.dut.initial_mask=mask; b.dut.eval(); w={}; w.valid_mask=mask;
    for (uint32_t lane=0; lane<32; ++lane) w.state[lane]=(mask>>lane)&1 ? LaneState::active : LaneState::exited;
    check(b,w);
}
void advance(TestBench& b, simt::Warp& w, uint32_t mask, uint32_t pc) {
    b.dut.retire_advance=1; b.dut.retire_mask=mask; b.dut.retire_pc=pc;
    for (uint32_t lane=0; lane<32; ++lane) if ((mask>>lane)&1) w.pc[lane]=pc+1;
    b.dut.eval();
}

// Use a sorted associative container as the independent selection reference, not a hardware reduction tree.
// 使用有序关联容器作为独立选择参考，不复用硬件归约树。
std::optional<simt::Group> probe(TestBench& b, simt::Warp& w, uint32_t policy) {
    std::map<uint32_t,uint32_t> groups;
    for (uint32_t lane=0; lane<32; ++lane) if (w.state[lane]==LaneState::active) groups[w.pc[lane]]|=1u<<lane;
    const bool resumed=groups.empty() && simt::resume_yielded(w);
    if (resumed) for (uint32_t lane=0; lane<32; ++lane) if (w.state[lane]==LaneState::active) groups[w.pc[lane]]|=1u<<lane;
    b.dut.policy=policy; b.dut.eval(); require(bool(b.dut.group_valid)==!groups.empty(),"group availability differs");
    require(bool(b.dut.needs_resume)==resumed,"idle yield detection differs");
    if (resumed) { b.dut.resume_idle=1; b.dut.eval(); }
    if (groups.empty()) return {};
    auto at=groups.begin();
    if (policy==2) at=std::prev(groups.end());
    else if (policy==0 && w.last_pc) { at=groups.upper_bound(*w.last_pc); if (at==groups.end()) at=groups.begin(); }
    require(b.dut.selected_pc==at->first && b.dut.selected_mask==at->second,"PC group selection differs");
    check(b,w); return simt::Group{at->first,at->second};
}
void issue(TestBench& b, simt::Warp& w, simt::Group group, bool advancing) {
    b.dut.issue_valid=1; b.dut.issue_pc=group.pc; b.dut.issue_mask=group.mask; b.dut.issue_advance=advancing;
    simt::resume_yielded(w); w.last_pc=group.pc;
    if (advancing) for (uint32_t lane=0; lane<32; ++lane) if ((group.mask>>lane)&1) w.pc[lane]=group.pc+1;
    b.dut.eval();
}

// External engines supply state/address/phase only; PCs are changed solely by instruction events.
// 外部引擎仅提供状态、地址和阶段，PC 只由指令事件修改。
void waits(TestBench& b, simt::Warp& w, uint32_t port, uint32_t mask, uint32_t state, uint32_t address, uint32_t phase) {
    const uint32_t offset=port*32; const uint64_t selected=uint64_t{mask}<<offset;
    b.dut.write_state_mask|=selected; b.dut.write_blocked_mask|=selected; b.dut.write_phase_mask|=selected;
    for (uint32_t lane=0; lane<32; ++lane) if ((mask>>lane)&1) {
        const auto index=offset+lane, shift=index*3;
        auto& words=b.dut.write_state;
        words[shift/32]=(words[shift/32]&~(7u<<(shift%32)))|(state<<(shift%32));
        if (shift%32>29) words[shift/32+1]=(words[shift/32+1]&~(7u>>(32-shift%32)))|(state>>(32-shift%32));
        b.dut.write_blocked[index]=address; b.dut.write_phase[index]=phase;
        w.state[lane]=static_cast<LaneState>(state); w.blocked_token[lane]=address; w.waiting_phase[lane]=phase;
    }
    b.dut.eval();
}
void control(TestBench& b, simt::Warp& w, const ControlCommand& c) {
    auto next=w;
    for (uint32_t lane=0; lane<32; ++lane) if ((c.group>>lane)&1) next.pc[lane]=c.pc+1;
    simt::Instruction i; i.op=static_cast<simt::Op>(c.opcode); i.rd=c.token; i.guard=c.guard; i.imm=c.target;
    simt::Module m; m.code.resize(c.code_size);
    if (c.target<m.code.size() && c.target_is_sync) { m.code[c.target].op=simt::Op::BSYNC; m.code[c.target].rd=c.token; }
    simt::Cta cta; simt::Config config; simt::Memory memory;
    const simt::ExecutionContext context{i,cta,w,next,config,0,memory,memory};
    bool rejected=false; try { simt::execute_control(context,{c.pc,c.group},c.mask,m); } catch (const std::exception&) { rejected=true; }
    auto& d=b.dut; d.opcode=c.opcode; d.token=c.token; d.guard=c.guard;
    d.group_pc=c.pc; d.group_mask=c.group; d.mask=c.mask; d.target_pc=c.target;
    d.code_size=c.code_size; d.target_is_sync=c.target_is_sync; d.command_valid=1; d.eval();
    require(bool(d.accept)!=rejected && bool(d.error)==rejected,"connected command acceptance differs");
    if (!rejected) w=next;
    check(b,w);
}

void initialization_forwarding_and_idle_storage() {
    TestBench b("frontend-init.vcd"); simt::Warp w;
    for (uint32_t mask : {0u,1u,0x80000001u,UINT32_MAX}) {
        initialize(b,w,mask); const auto group=probe(b,w,0);
        if (group) issue(b,w,*group,true);
        tick(b,w); require(!b.dut.clock_enable,"idle frontend did not gate updates");
        for (uint32_t idle=0; idle<8; ++idle) { b.dut.eval(); check(b,w); b.tick(); }
    }
}
void random_selection_wait_metadata_and_wrapping_pc() {
    TestBench b(nullptr); simt::Warp w; std::mt19937 rng(61834); initialize(b,w,UINT32_MAX); tick(b,w);
    for (uint32_t cycle=0; cycle<6000; ++cycle) {
        const uint32_t changed=rng(); waits(b,w,0,changed,rng()%6,rng(),rng());
        advance(b,w,uint32_t(rng()),cycle%17 ? uint32_t(rng()) : UINT32_MAX);
        if (cycle%3==0) waits(b,w,1,uint32_t(rng()),rng()%6,rng(),rng());
        const auto group=probe(b,w,cycle%3);
        if (group && rng()%4) issue(b,w,*group,bool(rng()%2));
        tick(b,w);
    }
}

// Connected reconvergence must consume stored lanes, release waiters and feed same-cycle selection.
// 直连重聚必须使用已存 lane，释放等待者，并旁路到同拍选择。
void nested_tokens_exit_and_same_cycle_issue() {
    TestBench b("frontend-control.vcd"); simt::Warp w; initialize(b,w,UINT32_MAX); tick(b,w);
    ControlCommand c{26,15,7,0,UINT32_MAX,UINT32_MAX,64,128,true}; control(b,w,c); tick(b,w);
    c={25,0,7,1,UINT32_MAX,0xffff,20,128,false}; control(b,w,c); tick(b,w);
    c={27,15,7,64,0xffff0000,0xffff0000,0,128,false}; control(b,w,c); tick(b,w);
    c={24,0,7,20,0xffff,0xffff,0,128,false}; control(b,w,c);
    require(b.dut.wake_mask==0xffff0000,"EXIT did not wake stored BSYNC lanes");
    const auto group=probe(b,w,0); require(group && group->pc==65 && group->mask==0xffff0000,"wake-to-select bypass");
    issue(b,w,*group,true); tick(b,w);
    c={29,0,7,66,0xffff0000,0xffff0000,0,128,false}; control(b,w,c); tick(b,w);
    const auto resumed=probe(b,w,0); require(resumed && resumed->pc==67,"yield-only wakeup"); tick(b,w);
}

void ordered_external_ports_errors_and_clear() {
    TestBench b("frontend-events.vcd"); simt::Warp w; initialize(b,w,UINT32_MAX); tick(b,w);
    waits(b,w,0,0xaaaaaaaa,4,0xfedcba98,0xffffffff);
    ControlCommand c{25,0,7,0,0x55555555,0x55555555,100,128,false}; control(b,w,c);
    waits(b,w,1,0xaaaaaaaa,0,0x12345678,17);
    const auto group=probe(b,w,2); require(group && group->pc==100,"control-to-selector data path");
    issue(b,w,*group,false); tick(b,w);
    c.target=128; control(b,w,c); require(b.dut.error==10,"wrong rejected branch error"); tick(b,w);
    initialize(b,w,0x1ffff); tick(b,w);
    require(w.waiting_phase[31]==0 && w.pc[0]==0 && !w.last_pc,"slot reuse leaked metadata");
    b.dut.reset=1; b.tick(); b.dut.reset=0; pins(b); w={}; w.state.fill(LaneState::exited); check(b,w);
}

// A dedicated release vector compares stored CTA IDs and cannot wake an asynchronous address with the same low bits.
// 专用释放向量比较已存 CTA 编号，不能唤醒低位相同的异步地址。
void cta_ports_immediate_release_and_wait_isolation() {
    TestBench b("frontend-cta.vcd"); simt::Warp w; initialize(b,w,UINT32_MAX); tick(b,w);
    advance(b,w,UINT32_MAX,3); b.dut.cta_block_mask=0xff; b.dut.cta_block_id=15;
    for (uint32_t lane=0; lane<8; ++lane) { w.state[lane]=LaneState::cta_barrier; w.blocked_token[lane]=15; }
    tick(b,w);
    waits(b,w,0,1u<<8,4,15,23); waits(b,w,0,1u<<9,3,0x1000f,17);
    b.dut.cta_release_mask=1u<<15;
    for (uint32_t lane=0; lane<8; ++lane) w.state[lane]=LaneState::active;
    b.dut.eval(); require(b.dut.cta_wake_mask==0xff,"CTA wake selected asynchronous or wide address");
    auto group=probe(b,w,0); require(group && group->mask==(UINT32_MAX^0x300),"CTA wake-to-select bypass");
    issue(b,w,*group,true); tick(b,w);
    advance(b,w,0x80000401,7); b.dut.cta_block_mask=0x80000401; b.dut.cta_block_id=7; b.dut.cta_release_mask=1u<<7;
    for (uint32_t lane : {0,10,31}) w.blocked_token[lane]=7;
    b.dut.eval(); require(b.dut.cta_wake_mask==0x80000401,"same-cycle CTA block/release lost waiters"); tick(b,w);
}

void run_cases() {
    initialization_forwarding_and_idle_storage(); std::cout<<"PASS frontend initialization, first issue, partial lanes and idle storage\n";
    random_selection_wait_metadata_and_wrapping_pc(); std::cout<<"PASS frontend 6000 random ordered cycles, selection policies, waits and PC wrap\n";
    nested_tokens_exit_and_same_cycle_issue(); std::cout<<"PASS frontend connected token storage, EXIT wakeup, same-cycle issue and yield\n";
    ordered_external_ports_errors_and_clear(); std::cout<<"PASS frontend pre/control/post ordering, rejected branch, slot reuse and reset\n";
    cta_ports_immediate_release_and_wait_isolation(); std::cout<<"PASS frontend CTA block/release ports, immediate wakeup and async isolation\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

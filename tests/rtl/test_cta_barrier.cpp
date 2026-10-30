#include "bench.hpp"
#include "Vcta_barrier.h"
#include "simt/memsync.hpp"
#include "simt/control.hpp"
#include "simt/cta_barrier.hpp"
#include <random>

namespace {
using TestBench=Bench<Vcta_barrier>;
using simt::hw::CtaBarrierCommand;
struct Reference { simt::Cta cta; simt::Config config; };
template<class Words> uint32_t counter(const Words& words,uint32_t id) {
    const auto shift=id*11; uint64_t bits=words[shift/32];
    if (shift%32>21) bits|=uint64_t{words[shift/32+1]}<<32;
    return (bits>>(shift%32))&2047;
}
uint32_t live_word(const Reference& r,uint32_t warp) {
    uint32_t mask=0;
    if (warp>=r.cta.warps.size()) return 0;
    for (uint32_t lane=0; lane<32; ++lane)
        if (r.cta.warps[warp].state[lane]!=simt::LaneState::exited) mask|=1u<<lane;
    return mask;
}

// Compare exact bitmap ownership, not merely a count that could hide duplicate or lost arrivals.
// 比较精确位图归属，而不只比较可能掩盖重复或丢失到达的计数。
void check(TestBench& b,const Reference& r) {
    uint32_t live=0;
    for (uint32_t wi=0; wi<32; ++wi) {
        const auto word=live_word(r,wi); live+=std::popcount(word);
        require(b.dut.observed_live[wi]==word,"live thread bitmap differs");
    }
    require(b.dut.observed_threads==r.config.threads && b.dut.observed_live_count==live,"live/launch count differs");
    for (uint32_t id=0; id<16; ++id) {
        const auto& record=r.cta.barriers[id]; uint32_t expected=0;
        require(bool((b.dut.active_mask>>id)&1)==record.active && counter(b.dut.counts,id)==record.count,"active/count differs");
        require(counter(b.dut.arrived_counts,id)==record.arrived.count(),"cached arrival count differs");
        const uint64_t generation=uint64_t{b.dut.generations[id*2]}|(uint64_t{b.dut.generations[id*2+1]}<<32);
        require(generation==record.generation,"generation differs");
        for (uint32_t wi=0; wi<32; ++wi) {
            uint32_t arrived=0;
            for (uint32_t lane=0; lane<32; ++lane) if (record.arrived[wi*32+lane]) arrived|=1u<<lane;
            require(b.dut.arrivals[id*32+wi]==arrived,"per-thread arrival bitmap differs");
            expected+=std::popcount(live_word(r,wi)|arrived);
        }
        if (record.active) require(counter(b.dut.expected_counts,id)==(record.count ? record.count : expected),"dynamic expected count differs");
    }
}
void initialize(TestBench& b,Reference& r,uint32_t threads) {
    r={}; r.config.threads=threads; r.cta.warps.resize((threads+31)/32);
    for (uint32_t wi=0; wi<r.cta.warps.size(); ++wi)
        for (uint32_t lane=0; lane<32; ++lane)
            r.cta.warps[wi].state[lane]=wi*32+lane<threads ? simt::LaneState::active : simt::LaneState::exited;
    b.dut.command_valid=0; b.dut.initialize=1; b.dut.initial_threads=threads; b.dut.eval(); check(b,r);
    b.tick(); b.dut.initialize=0; b.dut.eval(); check(b,r); require(b.dut.ready,"initialized CTA not ready");
}

// Use the pre-existing functional barrier and EXIT implementation as the independent transaction oracle.
// 使用已有功能屏障与 EXIT 实现作为独立事务参考。
Reference execute(const Reference& old,const CtaBarrierCommand& c) {
    Reference next=old;
    if (c.warp>=next.cta.warps.size()) {
        if (!c.exit && c.mask) throw std::runtime_error("barrier_inactive_lane");
        return next;
    }
    auto before=next.cta.warps[c.warp], after=before;
    simt::Instruction i; i.op=c.exit ? simt::Op::EXIT : simt::Op::BAR_ARRIVE; i.rd=c.barrier; i.imm=c.count;
    simt::Memory global,constant; simt::Module module;
    const simt::ExecutionContext context{i,next.cta,before,after,next.config,c.warp,global,constant};
    if (c.exit) simt::execute_control(context,{0,c.mask},c.mask,module);
    else simt::execute_barrier(context,c.mask);
    if (!c.exit && (c.mask&~live_word(old,c.warp))) throw std::runtime_error("barrier_inactive_lane");
    next.cta.warps[c.warp]=after; simt::release_cta_barriers(next.cta); return next;
}
uint32_t apply(TestBench& b,Reference& r,const CtaBarrierCommand& c) {
    auto next=r; std::string fault;
    try { next=execute(r,c); } catch (const std::exception& e) { fault=e.what(); }
    auto& d=b.dut; d.command_valid=1; d.is_exit=c.exit; d.warp=c.warp; d.mask=c.mask; d.barrier=c.barrier; d.requested_count=c.count; d.eval();
    static constexpr std::array messages{"","invalid_cta_threads","cta_barrier_uninitialized","cta_barrier_id_out_of_range",
        "invalid_barrier_count","barrier_count_mismatch","duplicate_barrier_arrival","barrier_arrival_overflow","barrier_inactive_lane"};
    require(d.error<messages.size() && fault==messages[d.error],"barrier error differs from functional reference");
    require(bool(d.accept)==fault.empty(),"barrier acceptance differs");
    uint32_t release=0;
    if (fault.empty()) {
        for (uint32_t id=0; id<16; ++id) if (r.cta.barriers[id].generation!=next.cta.barriers[id].generation) release|=1u<<id;
        r=std::move(next);
    }
    require(d.release_mask==release,"release vector differs"); check(b,r);
    b.tick(); d.command_valid=0; d.eval(); check(b,r); require(!d.release_mask,"stale release after withdrawal");
    return release;
}

void capacity_tail_warps_and_generation_reuse() {
    TestBench b("cta-capacity.vcd"); Reference r;
    for (uint32_t threads : {1,37,257,1024}) {
        initialize(b,r,threads);
        for (uint32_t generation=0; generation<3; ++generation)
            for (uint32_t id=0; id<16; ++id)
                for (uint32_t wi=0; wi<r.cta.warps.size(); ++wi) {
                    const auto released=apply(b,r,{false,wi,live_word(r,wi),id,generation%2 ? threads : 0});
                    require(released==(wi+1==r.cta.warps.size() ? 1u<<id : 0),"early or missing last-warp release");
                }
    }
}

// Exiting an already arrived thread must retain its contribution to all active dynamic barriers.
// 已到达线程退出时，必须保留它对所有活跃动态屏障的贡献。
void arrived_exits_and_sixteen_simultaneous_releases() {
    TestBench b("cta-exits.vcd"); Reference r; initialize(b,r,1024);
    for (uint32_t id=0; id<16; ++id) apply(b,r,{false,0,1,id,0});
    require(apply(b,r,{true,0,1,0,0})==0,"arrived EXIT released too early");
    for (uint32_t wi=0; wi<32; ++wi) {
        const auto released=apply(b,r,{true,wi,live_word(r,wi),0,0});
        require(released==(wi==31 ? 65535u : 0u),"simultaneous dynamic release differs");
    }
    initialize(b,r,64); apply(b,r,{false,0,UINT32_MAX,15,64});
    apply(b,r,{true,1,UINT32_MAX,0,0});
    require(r.cta.barriers[15].active && counter(b.dut.expected_counts,15)==64,"EXIT weakened explicit count");
}
void rejected_commands_inactive_masks_and_clear() {
    TestBench b("cta-errors.vcd"); Reference r; initialize(b,r,64);
    apply(b,r,{false,0,0xffff,15,64}); apply(b,r,{false,0,3,15,64});
    apply(b,r,{false,1,1,15,32}); apply(b,r,{false,1,1,15,65}); apply(b,r,{false,1,1,63,0});
    apply(b,r,{false,1,0,63,UINT32_MAX}); apply(b,r,{false,31,1,0,0});
    for (uint32_t threads : {0,2047}) {
        b.dut.initialize=1; b.dut.initial_threads=threads; b.dut.eval();
        require(b.dut.error==1 && !b.dut.clock_enable,"invalid initialization modified state");
        b.tick(); b.dut.initialize=0; b.dut.eval(); check(b,r);
    }
    initialize(b,r,32); apply(b,r,{false,0,UINT32_MAX,0,16});
    apply(b,r,{false,0,0xffff,0,16}); require(r.cta.barriers[0].generation==1,"rejection poisoned following valid command");
    b.dut.reset=1; b.tick(); b.dut.reset=0; b.dut.eval(); r={}; r.config.threads=0; check(b,r);
    require(!b.dut.ready,"reset left initialized state");
}
void randomized_arrivals_exits_and_cached_counts() {
    TestBench b(nullptr); Reference r; std::mt19937 rng(106731);
    for (uint32_t round=0; round<100; ++round) {
        initialize(b,r,1+rng()%1024);
        for (uint32_t event=0; event<32; ++event) {
            const uint32_t wi=rng()%r.cta.warps.size(), id=rng()%16;
            const auto& current=r.cta.barriers[id];
            const uint32_t count=current.active ? current.count : rng()%2 ? 0 : 1+rng()%r.config.threads;
            apply(b,r,{rng()%5==0,wi,uint32_t(rng())&live_word(r,wi),id,count});
        }
    }
}
void run_cases() {
    capacity_tail_warps_and_generation_reuse(); std::cout<<"PASS CTA table 16 barriers, 1024 threads, tails and generation reuse\n";
    arrived_exits_and_sixteen_simultaneous_releases(); std::cout<<"PASS CTA arrived EXIT accounting, sixteen releases and explicit counts\n";
    rejected_commands_inactive_masks_and_clear(); std::cout<<"PASS CTA atomic rejection, inactive masks, invalid initialization and reset\n";
    randomized_arrivals_exits_and_cached_counts(); std::cout<<"PASS CTA 3200 random arrivals/exits and independent cached-count checks\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

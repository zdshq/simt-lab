#include "Vcta_resources.h"
#include "bench.hpp"
#include <array>
#include <bit>
#include <map>
#include <optional>
#include <random>
#include <set>

namespace {
struct Grant { uint32_t slot,warps,registers,shared; };
struct Driver {
    Bench<Vcta_resources> b{nullptr};
    Driver() {
        auto& d=b.dut; d.cta_limit=64; d.warp_limit=32; d.register_limit=65536; d.shared_limit=65536;
        for (uint32_t w=0; w<32; ++w) for (uint32_t lane=0; lane<32; ++lane) state(w,lane,5);
        d.eval();
    }
    void state(uint32_t warp,uint32_t lane,uint32_t value) {
        const auto bit=(warp*32+lane)*3, word=bit/32, shift=bit%32;
        const uint64_t mask=uint64_t{7}<<shift, bits=uint64_t{value}<<shift;
        b.dut.lane_states[word]=(b.dut.lane_states[word]&~uint32_t(mask))|uint32_t(bits);
        if (shift>29) b.dut.lane_states[word+1]=(b.dut.lane_states[word+1]&~uint32_t(mask>>32))|uint32_t(bits>>32);
    }
    void tick() {
        b.tick(); auto& d=b.dut; d.request_valid=0; d.release_mask=0; d.begin_mask=0;
        d.initialize_mask=0; d.clean_mask=0; d.dma_start_mask=0; d.dma_finish_mask=0; d.eval();
    }
    bool offer(uint32_t id,uint32_t threads,uint32_t shared) {
        auto& d=b.dut; d.request_cta=id; d.request_threads=threads; d.request_shared=shared; d.eval();
        d.request_slot=d.grant_slot; d.request_warps=d.grant_warps; d.request_valid=1; d.eval();
        return d.accepted;
    }
    Grant allocate(uint32_t id,uint32_t threads,uint32_t shared=0) {
        require(offer(id,threads,shared) && !b.dut.error,"valid allocation rejected");
        const Grant g{b.dut.grant_slot,b.dut.grant_warps,b.dut.grant_registers,b.dut.grant_shared}; tick(); return g;
    }
    void query(uint32_t id) { b.dut.query_cta=id; b.dut.eval(); }
    void retire(uint32_t slot) {
        b.dut.begin_mask=uint64_t{1}<<slot; b.dut.eval(); require(!b.dut.error,"retirement start rejected"); tick();
        b.dut.release_mask=uint64_t{1}<<slot; b.dut.eval(); require(!b.dut.error,"retirement finish rejected"); tick();
    }
};

void rounding_capacity_and_partial_warps() {
    Driver t; auto& d=t.b.dut; d.cta_limit=2; d.warp_limit=3; d.register_limit=6144; d.shared_limit=512; d.eval();
    const auto first=t.allocate(0xf0000001,33,129);
    require(first.slot==0 && first.warps==3 && first.registers==4096 && first.shared==256,"rounding or physical slots wrong");
    t.state(1,31,0); d.eval(); t.query(0xf0000001);
    require(d.query_drained,"unused lanes prevented a partial warp from retiring");
    t.state(1,0,4); d.eval(); require(!d.query_drained,"live lane ignored"); t.state(1,0,5);
    d.inflight[1]=UINT32_MAX; d.eval(); require(!d.query_drained,"in-flight count narrowed or ignored"); d.inflight[1]=0; d.eval();
    require(!t.offer(2,33,0) && !d.error,"register exhaustion ignored"); t.tick();
    require(!t.offer(2,1,257) && !d.error,"rounded shared exhaustion ignored"); t.tick();
    const auto last=t.allocate(2,1,256); require(last.warps==4 && d.occupied_shared==512,"exact capacity rejected");
    require(!t.offer(3,1,0) && !d.error,"CTA limit ignored"); t.tick();
    t.retire(first.slot); t.retire(last.slot);
    require(!d.live && !d.occupied_registers && !d.occupied_shared,"resource credits leaked");
}

void full_warps_and_parallel_handoff() {
    Driver t; auto& d=t.b.dut;
    for (uint32_t w=0; w<32; ++w) {
        const auto grant=t.allocate(UINT32_MAX-w,32);
        require(grant.slot==w && grant.warps==(1u<<w),"full-capacity placement incorrect");
    }
    require(d.occupied_registers==65536 && d.occupied_warps==UINT32_MAX,"physical register capacity reduced");
    require(!t.offer(1,1,0) && !d.error,"accepted a 33rd physical warp"); t.tick();
    d.begin_mask=d.live; d.eval(); require(!d.error,"parallel retire starts rejected"); t.tick();
    d.release_mask=d.live; d.eval();
    require(t.offer(9,1024,65536),"same-edge full release and admission rejected"); t.tick();
    require(d.occupied_ctas==1 && d.occupied_warps==UINT32_MAX && d.occupied_shared==65536,"full handoff lost credits");
    t.query(9); require(d.query_warps==UINT32_MAX && d.query_shared==65536,"largest CTA metadata narrowed");
    d.begin_mask=uint64_t{1}<<63; d.eval(); require(d.error==5,"unowned high physical slot accepted"); t.tick();
    require(d.occupied_ctas==1,"invalid high-slot event changed live state"); t.retire(0);
    require(t.offer(0xdeadbeef,1,65536),"high-slot request rejected");
    d.request_slot=63; d.request_warps=uint32_t{1}<<31; d.eval();
    require(d.accepted,"held high-slot grant rejected"); t.tick(); t.query(0xdeadbeef);
    require(d.query_slot==63 && d.query_shared==65536 && d.query_warps==(uint32_t{1}<<31),"high-slot query narrowed");
    d.dma_start_mask=uint64_t{1}<<63; d.eval(); t.tick(); require(d.query_dma==1,"high-slot DMA query wrong");
    d.dma_finish_mask=uint64_t{1}<<63; d.eval(); t.tick(); t.retire(63);
}

void held_grant_survives_later_release() {
    Driver t; auto& d=t.b.dut;
    for (uint32_t id=0; id<4; ++id) t.allocate(id,32,128);
    d.begin_mask=9; d.eval(); t.tick(); d.release_mask=8; d.eval();
    require(t.offer(77,32,128) && d.grant_slot==3 && d.grant_warps==8,"expected high reusable slot");
    d.release_mask|=1; d.eval();
    t.state(3,0,0); d.eval();
    require(d.accepted && d.grant_slot==3 && d.grant_warps==8,"late release or new lane wires retargeted held grant"); t.tick();
    t.query(77); require(d.query_found && d.query_slot==3 && d.query_warps==8,"committed grant changed");
    require(!d.query_drained,"new active owner was already drained"); t.state(3,0,5); d.eval();
    const auto next=t.allocate(88,32,128); require(next.slot==0 && next.warps==1,"new free slot not reusable");
    for (auto id : {77u,88u,1u,2u}) { t.query(id); t.retire(d.query_slot); }
}

void asynchronous_retirement_and_invalid_batches() {
    Driver t; auto& d=t.b.dut; t.allocate(0xffffffff,1); t.query(0xffffffff);
    d.initialize_mask=1; d.dma_start_mask=1; d.eval(); require(!d.error,"initial table/DMA events rejected"); t.tick();
    require(d.query_dma==1 && !d.query_drained && d.query_table_used,"DMA or table ownership missing");
    for (uint32_t n=1; n<64; ++n) { d.dma_start_mask=1; d.eval(); require(!d.error,"DMA counter truncated"); t.tick(); }
    d.dma_start_mask=1; d.clean_mask=1; d.eval(); require(d.error==5,"overflow/early cleanup accepted"); t.tick();
    require(d.query_dma==64 && !d.query_table_clean,"invalid batch changed state");
    d.dma_start_mask=1; d.dma_finish_mask=1; d.eval(); require(!d.error,"simultaneous DMA replacement rejected"); t.tick();
    require(d.query_dma==64,"simultaneous DMA count wrong");
    for (uint32_t n=0; n<64; ++n) { d.dma_finish_mask=1; d.eval(); require(!d.error,"DMA finish rejected"); t.tick(); }
    require(d.query_drained && d.cleanup==1,"drained CTA did not request table cleanup");
    d.begin_mask=1; d.eval(); require(d.error==5,"retired before transaction cleanup"); t.tick();
    d.clean_mask=1; d.eval(); require(!d.error,"table cleanup completion rejected"); t.tick();
    require(d.query_table_clean && !d.cleanup,"completed cleanup offered again"); t.retire(0);
    t.allocate(2,1); t.query(2); d.dma_finish_mask=1; d.eval(); require(d.error==5,"DMA underflow accepted"); t.tick();
    require(!d.query_dma,"underflow changed counter"); d.dma_start_mask=1; d.eval(); t.tick();
    d.reset=1; t.tick(); d.reset=0; d.eval(); require(!d.live && !d.occupied_registers,"reset retained ownership");
}

struct Reference { uint32_t slot,mask,shared; };
void compare_owners(Driver& t,const std::map<uint32_t,Reference>& owners) {
    uint32_t mask=0,shared=0; uint64_t live=0;
    for (const auto& [id,owner] : owners) {
        mask|=owner.mask; shared+=owner.shared; live|=uint64_t{1}<<owner.slot; t.query(id);
        require(t.b.dut.query_found && t.b.dut.query_slot==owner.slot && t.b.dut.query_warps==owner.mask &&
                t.b.dut.query_shared==owner.shared,"random ownership mismatch");
    }
    const auto& d=t.b.dut;
    require(d.live==live && d.occupied_warps==mask && d.occupied_ctas==owners.size() &&
            d.occupied_shared==shared && d.occupied_registers==std::popcount(mask)*2048u,"random credit conservation failed");
}
std::optional<Reference> reference_grant(const std::map<uint32_t,Reference>& owners,uint32_t threads,uint32_t shared) {
    std::set<uint32_t> free_warps,free_ctas;
    for (uint32_t n=0; n<16; ++n) free_warps.insert(n);
    for (uint32_t n=0; n<12; ++n) free_ctas.insert(n);
    uint32_t used=0;
    for (const auto& [id,owner] : owners) {
        static_cast<void>(id); used+=owner.shared; free_ctas.erase(owner.slot);
        for (uint32_t w=0; w<32; ++w) if (owner.mask & (1u<<w)) free_warps.erase(w);
    }
    const auto count=(threads+31)/32, bytes=(shared+127)/128*128;
    if (free_ctas.empty() || free_warps.size()<count || used+bytes>4096) return {};
    Reference result{*free_ctas.begin(),0,bytes};
    for (uint32_t n=0; n<count; ++n) { result.mask|=1u<<*free_warps.begin(); free_warps.erase(free_warps.begin()); }
    return result;
}
void random_resource_reference() {
    Driver t; auto& d=t.b.dut; d.cta_limit=12; d.warp_limit=16; d.register_limit=32768; d.shared_limit=4096; d.eval();
    std::mt19937 rng(71303); std::map<uint32_t,Reference> owners;
    for (uint32_t n=0; n<1500; ++n) {
        if (!owners.empty() && rng()%3==0) {
            auto at=owners.begin(); std::advance(at,rng()%owners.size()); t.retire(at->second.slot); owners.erase(at);
        } else {
            const uint32_t id=0x80000000u+n*13, threads=1+rng()%160, shared=rng()%1100;
            const auto expected=reference_grant(owners,threads,shared); const auto accepted=t.offer(id,threads,shared);
            require(!d.error && accepted==bool(expected),"random admission differs from free-list reference");
            if (expected) { require(d.grant_slot==expected->slot && d.grant_warps==expected->mask,"random placement mismatch"); owners.emplace(id,*expected); }
            t.tick();
        }
        compare_owners(t,owners);
    }
    while (!owners.empty()) { t.retire(owners.begin()->second.slot); owners.erase(owners.begin()); }
    compare_owners(t,owners);
    require(!t.offer(1,0,0) && d.error==2,"zero-thread request accepted"); t.tick();
    t.allocate(1,1); require(!t.offer(1,1,0) && d.error==3,"duplicate CTA ID accepted"); t.tick();
    d.begin_mask=1; d.release_mask=1; d.eval(); require(d.error==5,"two-phase retirement collapsed"); t.tick();
    t.retire(0);
}
int run_application() {
    try {
        run_case("CTA resource rounding, capacity and partial-warp eligibility",rounding_capacity_and_partial_warps);
        run_case("CTA resource full 32-warps and parallel credit handoff",full_warps_and_parallel_handoff);
        run_case("CTA resource held allocation survives a later release",held_grant_survives_later_release);
        run_case("CTA resource DMA/table retirement, invalid batches and reset",asynchronous_retirement_and_invalid_batches);
        run_case("CTA resource 1500 randomized requests against independent free lists",random_resource_reference); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

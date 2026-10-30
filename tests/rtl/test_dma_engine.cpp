#include "Vdma_engine.h"
#include "bench.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <random>
#include <vector>

namespace {
using TestBench=Bench<Vdma_engine>;
struct Copy {
    uint64_t id;
    uint32_t source, destination, bytes, cta, barrier, phase;
    std::vector<uint8_t> data, read, written;
    uint32_t acknowledged=0;
};
struct Return {
    uint32_t slot, bytes;
    uint64_t id;
    std::array<uint32_t,32> data{};
};
struct Completion { uint32_t slot; uint64_t id; };

template<class Words> uint64_t address(const Words& words,uint32_t lane) {
    uint64_t value=0;
    for (uint32_t bit=0; bit<33; ++bit)
        value|=uint64_t{(words[(lane*33+bit)/32]>>((lane*33+bit)%32))&1}<<bit;
    return value;
}

// The oracle tracks bytes, ownership and acknowledgements; it does not reproduce the fragment scheduler.
// 参考模型仅追踪字节、归属与确认，不复制片段调度算法。
class Driver {
public:
    TestBench b{nullptr};
    std::map<uint64_t,Copy> copies;
    std::vector<Return> reads, writes;
    std::vector<Completion> completed;
    uint32_t claims=0, releases=0;
    uint64_t slots=0;
    explicit Driver(uint32_t commands=64,uint32_t fragments=64,uint32_t chunk=128) {
        b.dut.command_limit=commands; b.dut.fragment_limit=fragments; b.dut.chunk_bytes=chunk; b.dut.eval();
    }
    void edge() {
        b.tick(); b.dut.command_valid=0; b.dut.read_ready=0; b.dut.write_ready=0;
        b.dut.read_response_valid=0; b.dut.write_response_valid=0;
        b.dut.completion_ready=0; b.dut.release_valid=0; b.dut.eval();
    }
    void submit(uint64_t id,uint32_t source,uint32_t destination,uint32_t bytes) {
        Copy c{id,source,destination,bytes,uint32_t(id*37),uint32_t(id*32),uint32_t(id>>32),{},{},{}};
        c.data.resize(bytes); c.read.resize(bytes); c.written.resize(bytes);
        for (uint32_t i=0; i<bytes; ++i) c.data[i]=uint8_t((i*137+id*71)^(i>>3));
        auto& d=b.dut; d.command_valid=1; d.command_id=id; d.command_cta=c.cta;
        d.command_source=source; d.command_destination=destination; d.command_bytes=bytes;
        d.command_barrier=c.barrier; d.command_phase=c.phase; d.eval();
        require(d.command_accept && !d.error,"valid descriptor rejected"); copies.emplace(id,std::move(c)); edge();
    }
    Return sample(bool write) {
        auto& d=b.dut; Return result{uint32_t(write ? d.write_slot : d.read_slot),0,write ? d.write_copy : d.read_copy,{}};
        auto& c=copies.at(result.id); const uint32_t width=(write ? d.write_word : d.read_word) ? 4 : 1;
        const auto mask=write ? d.write_mask : d.read_mask;
        require(mask && (write ? d.write_cta : d.read_cta)==c.cta,"request lost CTA or mask");
        for (uint32_t lane=0; lane<32; ++lane) {
            if (!(mask&(uint32_t{1}<<lane))) continue;
            const auto at=write ? address(d.write_addresses,lane) : address(d.read_addresses,lane);
            const uint64_t base=write ? c.destination : c.source;
            require(at>=base && at+width<=base+c.bytes,"request outside descriptor");
            for (uint32_t byte=0; byte<width; ++byte) {
                const auto offset=at-base+byte; auto& seen=write ? c.written[offset] : c.read[offset];
                require(!seen,"byte requested twice"); seen=1; ++result.bytes;
                if (write) require(uint8_t(d.write_data[lane]>>(byte*8))==c.data[offset],"returned payload corrupted or crossed owners");
                else result.data[lane]|=uint32_t{c.data[offset]}<<(byte*8);
            }
        }
        require(result.bytes<=b.dut.chunk_bytes,"chunk budget exceeded"); return result;
    }
    void accept(bool write) {
        auto& d=b.dut; if (!(write ? d.write_valid : d.read_valid)) return;
        auto& pending=write ? writes : reads; const auto r=sample(write);
        require(std::none_of(pending.begin(),pending.end(),[&](const auto& p){return p.slot==r.slot;}),"slot accepted twice");
        pending.push_back(r); slots|=uint64_t{1}<<r.slot;
        if (write) d.write_ready=1; else d.read_ready=1;
    }
    void respond(bool write,uint32_t index) {
        auto& d=b.dut; auto& pending=write ? writes : reads; const auto r=pending.at(index);
        if (write) { d.write_response_valid=1; d.write_response_slot=r.slot; }
        else {
            d.read_response_valid=1; d.read_response_slot=r.slot;
            for (uint32_t lane=0; lane<32; ++lane) d.read_response_data[lane]=r.data[lane];
        }
        d.eval(); require(write ? d.write_response_ready : d.read_response_ready,"return rejected");
        require((write ? d.write_response_copy : d.read_response_copy)==r.id,"return owner differs");
        require((write ? d.write_response_bytes : d.read_response_bytes)==r.bytes,"return byte count differs");
        if (write) copies.at(r.id).acknowledged+=r.bytes;
        pending.erase(pending.begin()+index);
    }
    void claim() {
        auto& d=b.dut; if (!d.completion_valid) return;
        const auto& c=copies.at(d.completion_id);
        require(c.acknowledged==c.bytes,"completion preceded all write acknowledgements");
        require(d.completion_cta==c.cta && d.completion_barrier==c.barrier && d.completion_phase==c.phase &&
                d.completion_bytes==c.bytes,"completion descriptor changed");
        require(std::all_of(c.read.begin(),c.read.end(),[](auto v){return v==1;}) &&
                std::all_of(c.written.begin(),c.written.end(),[](auto v){return v==1;}),"copy has byte holes");
        completed.push_back({d.completion_slot,d.completion_id}); d.completion_ready=1; ++claims;
    }
    void release(uint32_t index) {
        const auto c=completed.at(index); auto& d=b.dut;
        d.release_valid=1; d.release_slot=c.slot; d.release_id=c.id; d.eval();
        require(d.release_ready,"claimed command not releasable"); ++releases;
        completed.erase(completed.begin()+index); copies.erase(c.id);
    }
    void step(std::mt19937& rng,bool free=true) {
        if (free && !completed.empty() && rng()%3) release(uint32_t(rng()%completed.size()));
        if (!reads.empty() && rng()%3) respond(false,uint32_t(rng()%reads.size()));
        if (!writes.empty() && rng()%3) respond(true,uint32_t(rng()%writes.size()));
        if (rng()%3) accept(false);
        if (rng()%3) accept(true);
        if (rng()%3) claim();
        edge(); require(!b.dut.error,"unexpected DMA protocol error");
    }
    void drain(std::mt19937& rng) {
        for (uint32_t i=0; i<200000 && !copies.empty(); ++i) step(rng);
        require(copies.empty() && reads.empty() && writes.empty() && completed.empty(),"DMA failed to drain");
        require(!b.dut.command_occupancy && !b.dut.fragment_occupancy && !b.dut.clock_enable,"credits or idle clock leaked");
    }
};

void wait_read(Driver& t) {
    for (uint32_t i=0; i<100 && !t.b.dut.read_valid; ++i) t.edge();
    require(t.b.dut.read_valid,"missing read offer");
}
void hold_read(Driver& t,uint32_t clocks) {
    auto& d=t.b.dut; const auto slot=d.read_slot; const auto id=d.read_copy;
    const auto first=address(d.read_addresses,0); const auto mask=d.read_mask;
    for (uint32_t i=0; i<clocks; ++i) {
        t.edge(); require(d.read_valid && d.read_slot==slot && d.read_copy==id &&
            address(d.read_addresses,0)==first && d.read_mask==mask,"stalled read changed");
    }
}
void completion_requires_publication_release() {
    Driver t(1,1,128); std::mt19937 rng(5); t.submit(0x100000001ull,4096,128,128); wait_read(t); hold_read(t,13);
    t.accept(false); t.edge(); t.respond(false,0); t.edge();
    for (uint32_t i=0; i<20; ++i) t.edge();
    require(t.b.dut.fragment_occupancy==1 && !t.b.dut.completion_valid,"read return freed fragment or completed command");
    t.accept(true); t.edge();
    for (uint32_t i=0; i<20; ++i) { t.edge(); require(!t.b.dut.completion_valid,"write issue completed command"); }
    t.respond(true,0); t.edge();
    for (uint32_t i=0; i<20 && !t.b.dut.completion_valid; ++i) t.edge();
    require(t.b.dut.completion_valid && !t.b.dut.command_ready && !t.b.dut.fragment_occupancy,"completion ownership wrong");
    const auto id=t.b.dut.completion_id;
    for (uint32_t i=0; i<12; ++i) { t.edge(); require(t.b.dut.completion_valid && t.b.dut.completion_id==id,"completion not held"); }
    t.claim(); t.edge();
    for (uint32_t i=0; i<12; ++i) { t.edge(); require(!t.b.dut.command_ready && t.b.dut.command_occupancy==1,"claim returned credit early"); }
    t.drain(rng); require(t.claims==1 && t.releases==1,"completion repeated");
}
void full_pools_and_reversed_returns() {
    Driver t(64,64,64); std::mt19937 rng(71);
    for (uint32_t i=0; i<64; ++i) t.submit(0x123400000000ull+i,4096+i*128,256+i*128,64);
    wait_read(t); hold_read(t,80); require(t.b.dut.command_occupancy==64 && t.b.dut.fragment_occupancy==64,"physical pools not full");
    for (uint32_t i=0; i<200 && t.reads.size()!=64; ++i) { t.accept(false); t.edge(); }
    require(t.reads.size()==64 && t.slots==UINT64_MAX,"highest fragment slot not exercised");
    while (!t.reads.empty()) { t.respond(false,uint32_t(t.reads.size()-1)); t.edge(); }
    require(t.b.dut.fragment_occupancy==64,"read returns released staging storage");
    const auto slot=t.b.dut.write_slot; std::array<uint32_t,32> payload{};
    std::copy_n(std::begin(t.b.dut.write_data),32,payload.begin());
    for (uint32_t i=0; i<17; ++i) { t.edge(); require(t.b.dut.write_slot==slot &&
        std::equal(payload.begin(),payload.end(),std::begin(t.b.dut.write_data)),"stalled write changed payload"); }
    t.drain(rng); require(t.claims==64 && t.releases==64,"full command pool did not complete once");
}
void byte_tails_and_carries() {
    std::mt19937 rng(873);
    for (const auto chunk : {1u,3u,7u,32u,127u,128u}) {
        Driver t(1,1,chunk);
        for (const auto bytes : {1u,2u,3u,4u,5u,31u,32u,33u,127u,128u,129u,4095u,4096u}) {
            t.submit(uint64_t{chunk}*8192+bytes,0xfffffff8u+(bytes%4),0xffffffebu+(bytes%4),bytes); t.drain(rng);
        }
    }
}
void rejection_and_midflight_reset() {
    Driver t(2,2,128); auto& d=t.b.dut;
    for (auto bytes : {0u,4097u,UINT32_MAX}) {
        d.command_valid=1; d.command_bytes=bytes; d.eval(); require(!d.command_ready && d.error==2,"invalid size accepted"); t.edge();
    }
    t.submit(42,0,128,256); d.command_id=42; d.command_bytes=1; d.command_valid=1; d.eval();
    require(!d.command_ready && d.error==3,"duplicate owner accepted"); t.edge();
    d.read_response_valid=1; d.read_response_slot=63; d.eval(); require(!d.read_response_ready && d.error==4,"free read return accepted"); t.edge();
    d.write_response_valid=1; d.write_response_slot=63; d.eval(); require(!d.write_response_ready && d.error==5,"free write return accepted"); t.edge();
    d.release_valid=1; d.release_slot=0; d.release_id=42; d.eval(); require(!d.release_ready && d.error==6,"unclaimed release accepted"); t.edge();
    wait_read(t); t.accept(false); t.edge(); d.reset=1; t.edge(); d.reset=0; d.eval();
    require(!d.command_occupancy && !d.fragment_occupancy && !d.read_valid && !d.write_valid && !d.completion_valid,"reset retained ownership");
    t.copies.clear(); t.reads.clear(); t.submit(42,0,128,4); std::mt19937 rng(7); t.drain(rng);
}
void random_interleaving_and_reuse() {
    std::mt19937 rng(917503);
    for (auto chunk : {17u,128u}) {
        Driver t(37,53,chunk); uint32_t submitted=0;
        for (uint32_t i=0; i<100000 && (submitted<180 || !t.copies.empty()); ++i) {
            if (submitted<180 && t.b.dut.command_ready && rng()%2) {
                const uint64_t id=0x987600000000ull+submitted;
                t.submit(id,uint32_t(rng()),uint32_t(rng()),uint32_t(rng()%512)+1); ++submitted;
            } else t.step(rng);
        }
        require(submitted==180,"random descriptors stalled"); t.drain(rng);
        require(t.claims==180 && t.releases==180,"random completion count differs");
    }
}
void round_robin_fragment_fairness() {
    Driver t(4,64,128); std::mt19937 rng(93);
    for (uint32_t i=0; i<4; ++i) t.submit(i,0,256,4096);
    for (uint32_t i=0; i<200 && t.reads.size()!=64; ++i) { t.accept(false); t.edge(); }
    std::array<uint32_t,4> counts{};
    for (const auto& r : t.reads) ++counts.at(r.id);
    require(*std::min_element(counts.begin(),counts.end())>=15 && *std::max_element(counts.begin(),counts.end())<=17,
        "command rotation starved a long copy");
    t.drain(rng);
}
int run_application() {
    try {
        run_case("DMA read/write acknowledgements and delayed publication credit",completion_requires_publication_release);
        run_case("DMA full 64 command/fragment pools, stable offers and reversed returns",full_pools_and_reversed_returns);
        run_case("DMA byte tails, 1..128 byte chunks and 33-bit address carries",byte_tails_and_carries);
        run_case("DMA rejected descriptors/returns/releases and reset during flight",rejection_and_midflight_reset);
        run_case("DMA 360 randomized concurrent descriptors, payloads and slot reuse",random_interleaving_and_reuse);
        run_case("DMA round-robin fairness across long concurrent copies",round_robin_fragment_fairness); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

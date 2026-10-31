#include "Vtmem_memory.h"
#include "bench.hpp"
#include <array>
#include <bit>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {
struct Access { uint32_t mask=UINT32_MAX; bool write=false; std::array<uint32_t,32> words{},values{}; };
struct Driver {
    Bench<Vtmem_memory> b{nullptr};
    Driver() { b.dut.column_limit=256; b.dut.response_latency=2; b.dut.eval(); }
    void tick() {
        require(!b.dut.error,"unexpected TMEM protocol error "+std::to_string(b.dut.error));
        b.tick(); clear();
    }
    void clear() {
        auto& d=b.dut; d.command_valid=0; d.retire_ctas=0; d.request_valid=0; d.acknowledge=0; d.eval();
    }
    bool command(uint32_t cta,uint32_t count,bool allocate=true,uint32_t base=0) {
        auto& d=b.dut; d.command_cta=cta; d.command_count=count; d.command_base=base; d.command_allocate=allocate; d.eval();
        d.held_base=d.grant_base; d.held_pages=d.grant_pages; d.command_valid=1; d.eval(); return d.command_accepted;
    }
    uint32_t allocate(uint32_t cta,uint32_t count) {
        require(command(cta,count),"allocation failed: "+std::to_string(b.dut.error));
        const uint32_t base=b.dut.grant_base*8; tick(); return base;
    }
    void free(uint32_t cta,uint32_t base,uint32_t count) {
        require(command(cta,count,false,base),"free failed"); tick();
    }
    void retire(uint32_t cta) { b.dut.retire_ctas=uint64_t{1}<<cta; b.dut.eval(); tick(); }
    void query(uint32_t cta,uint32_t row,uint32_t column) {
        auto& d=b.dut; d.coordinate_cta=cta; d.coordinate_row=row; d.coordinate_column=column; d.eval();
    }
    uint32_t submit(uint32_t cta,const Access& a) {
        auto& d=b.dut; require(d.request_ready && !d.request_valid,"request port unavailable");
        d.request_cta=cta; d.request_mask=a.mask; d.request_write=a.write;
        for (auto& word : d.request_words) word=0;
        for (uint32_t lane=0; lane<32; ++lane) {
            const auto bit=lane*15,shift=bit%32; d.request_words[bit/32]|=a.words[lane]<<shift;
            if (shift>17) d.request_words[bit/32+1]|=a.words[lane]>>(32-shift);
            d.request_values[lane]=a.values[lane];
        }
        d.request_index=d.request_slot; d.request_valid=1; d.eval(); return d.request_slot;
    }
    std::array<uint32_t,32> consume(uint32_t slot) {
        auto& d=b.dut; require(d.completed & (1u<<slot),"response not complete"); d.query_slot=slot; d.eval();
        std::array<uint32_t,32> result{}; for (uint32_t lane=0; lane<32; ++lane) result[lane]=d.query_data[lane];
        d.acknowledge|=1u<<slot; d.eval(); return result;
    }
    std::array<uint32_t,32> wait(uint32_t slot) {
        for (uint32_t clocks=0; clocks<10000; ++clocks) {
            if (b.dut.completed & (1u<<slot)) { auto result=consume(slot); tick(); return result; }
            tick();
        }
        throw std::runtime_error("TMEM response timeout");
    }
    std::array<uint32_t,32> access(uint32_t cta,const Access& a) { auto slot=submit(cta,a); tick(); return wait(slot); }
};
Access vector(uint32_t base,bool write,uint32_t salt=0) {
    Access a; a.write=write;
    for (uint32_t lane=0; lane<32; ++lane) { a.words[lane]=base+lane; a.values[lane]=(base+lane)*0x9e3779b9u+salt; }
    return a;
}

// Traverse every physical word with a deterministic independent address-derived reference.
// 用独立的地址派生参考遍历每个物理字。
void full_capacity_and_synchronous_storage() {
    Driver t; auto& d=t.b.dut; require(t.allocate(63,256)==0 && d.used_columns==256,"full capacity unavailable");
    for (uint32_t word=0; word<32768; word+=32) t.access(63,vector(word,true,71));
    for (uint32_t word=0; word<32768; word+=32) {
        const auto a=vector(word,false,71); require(t.access(63,a)==a.values,"full-capacity SRAM contents mismatch");
    }
    t.query(63,127,255); require(!d.coordinate_error && d.coordinate_address==131068,"highest coordinate truncated");
    t.retire(63); require(!d.used_columns,"retirement leaked columns");
}
void multi_cta_mapping_fragmentation_and_exact_free() {
    Driver t; auto& d=t.b.dut;
    require(t.allocate(0,8)==0 && t.allocate(63,8)==0,"separate CTA logical windows not independent");
    t.access(0,vector(0,true,19)); t.access(63,vector(0,true,79));
    require(t.access(0,vector(0,false,19))==vector(0,false,19).values,"CTA zero-page alias");
    require(t.access(63,vector(0,false,79))==vector(0,false,79).values,"high CTA alias");
    require(t.allocate(0,16)==8,"logical first-fit wrong"); t.free(0,0,8);
    require(t.allocate(0,8)==0,"logical hole not reused");
    require(!t.command(0,8,false,8) && d.error==4,"partial allocation free accepted"); t.b.tick(); t.clear();
    require(d.used_columns==32,"invalid free partially changed allocation");
    t.query(0,0,24); require(d.coordinate_error==2,"unallocated column valid");
    t.query(63,128,0); require(d.coordinate_error==1,"invalid row accepted");
    t.query(63,0,UINT32_MAX); require(d.coordinate_error==1,"column width truncated");
    t.retire(0); require(d.used_columns==8,"retirement freed another owner");
    require(t.access(63,vector(0,false,79))==vector(0,false,79).values,"retirement corrupted live owner"); t.retire(63);
}
void bank_conflicts_broadcast_and_capacity() {
    Driver t; auto& d=t.b.dut; t.allocate(2,256);
    const auto start=t.b.cycle; t.access(2,vector(0,true,1)); const auto parallel=t.b.cycle-start;
    Access conflict; conflict.write=true;
    for (uint32_t lane=0; lane<32; ++lane) { conflict.words[lane]=lane*128; conflict.values[lane]=lane+17; }
    const auto before=t.b.cycle; t.access(2,conflict);
    require(t.b.cycle-before-parallel==32-(32+SIMT_TMEM_BANKS-1)/SIMT_TMEM_BANKS,"bank conflict latency wrong");
    Access broadcast; require(t.access(2,broadcast)==std::array<uint32_t,32>{17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17},"same-word broadcast wrong");
    std::array<uint32_t,8> slots{};
    for (uint32_t n=0; n<8; ++n) { auto a=vector(n*32,true,91); slots[n]=t.submit(2,a); t.tick(); }
    for (uint32_t n=0; n<300; ++n) t.tick();
    require(d.occupied==255 && d.completed==255 && !d.request_ready,"unconsumed responses did not retain capacity");
    t.consume(slots[7]); auto a=vector(224,false,91); const auto next=t.submit(2,a); t.consume(slots[0]);
    require(next==7 && d.request_slot==7,"late consumption changed held slot"); t.tick();
    for (uint32_t n=1; n<7; ++n) t.consume(slots[n]); t.tick();
    require(t.wait(next)==a.values,"response reuse corrupted data"); t.retire(2);
}
void invalid_batches_retirement_and_reset() {
    Driver t; auto& d=t.b.dut;
    for (auto count : {0u,1u,257u,UINT32_MAX}) {
        require(!t.command(0,count) && d.error==2,"invalid allocation accepted"); t.b.tick(); t.clear(); require(!d.used_columns,"invalid allocation committed");
    }
    d.column_limit=8; d.eval(); t.allocate(0,8);
    require(!t.command(63,8) && d.error==3,"global column quota ignored"); t.b.tick(); t.clear();
    auto a=vector(0,true,22); const auto slot=t.submit(0,a); t.tick(); d.retire_ctas=1; d.eval();
    require(d.error==5,"outstanding request owner retired"); t.b.tick(); t.clear();
    require(d.used_columns==8 && d.occupied,"invalid batch changed storage state"); t.wait(slot);
    a.words[31]=1024; t.submit(0,a); require(d.error==9,"unmapped lane accepted"); t.b.tick(); t.clear();
    require(t.access(0,vector(0,false,22))==vector(0,false,22).values,"invalid vector wrote a prefix");
    t.submit(0,vector(0,true,31)); t.tick(); d.reset=1; t.b.tick(); t.clear(); d.reset=0; d.eval();
    require(!d.occupied && !d.used_columns,"reset retained requests or mapping");
}

// Random data uses byte-independent word arrays; mapping decisions use a separate set allocator.
// 随机数据使用独立字数组，映射决策使用单独的集合分配参考。
void random_data_and_mapping_reference() {
    Driver t; std::mt19937 rng(82107); std::array<std::vector<uint32_t>,4> memories;
    for (uint32_t owner=0; owner<4; ++owner) {
        t.allocate(owner,64); memories[owner].resize(8192);
        for (uint32_t base=0; base<8192; base+=32) { auto a=vector(base,true); a.values.fill(0); t.access(owner,a); }
    }
    for (uint32_t iteration=0; iteration<700; ++iteration) {
        const auto owner=rng()%4; Access a; a.mask=rng(); a.write=rng()%2;
        for (uint32_t lane=0; lane<32; ++lane) { a.words[lane]=rng()%8192; a.values[lane]=rng(); }
        const auto actual=t.access(owner,a);
        for (uint32_t lane=0; lane<32; ++lane) if (a.mask & (1u<<lane)) {
            if (a.write) memories[owner][a.words[lane]]=a.values[lane];
            else require(actual[lane]==memories[owner][a.words[lane]],"random TMEM read differs from reference");
        }
    }
    for (uint32_t owner=0; owner<4; ++owner) {
        for (uint32_t base=0; base<8192; base+=32) {
            const auto actual=t.access(owner,vector(base,false));
            for (uint32_t lane=0; lane<32; ++lane) require(actual[lane]==memories[owner][base+lane],"final full data reference mismatch");
        }
        t.retire(owner);
    }
    require(!t.b.dut.used_columns,"random owners leaked allocation");
}
int run_application() {
    try {
        run_case("TMEM full 128 KiB SRAM and highest coordinates",full_capacity_and_synchronous_storage);
        run_case("TMEM multi-CTA mapping, logical holes and exact free",multi_cta_mapping_fragmentation_and_exact_free);
        run_case("TMEM bank conflicts, broadcast and held response slots",bank_conflicts_broadcast_and_capacity);
        run_case("TMEM invalid batches, live-owner retirement and reset",invalid_batches_retirement_and_reset);
        run_case("TMEM 700 random vectors and complete independent data reference",random_data_and_mapping_reference); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

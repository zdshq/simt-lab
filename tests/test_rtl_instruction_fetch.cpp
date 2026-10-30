#include "simt/instruction_fetch.hpp"
#include "simt/hardware_context.hpp"
#include "simt/cache_engine.hpp"
#include "hardware_support.hpp"

namespace simt::hw::test {
std::vector<InstructionWords> image(uint32_t count=1024) {
    std::vector<InstructionWords> result(count);
    for (uint32_t pc=0; pc<count; ++pc) result[pc]={pc,pc^0xdeadbeefu,pc*7,0x80000000u};
    return result;
}
HierarchyConfig configuration(bool rtl,uint32_t capacity=64) {
    HierarchyConfig result; result.cache_rtl=rtl; result.capacity=capacity; return result;
}
struct FetchRig {
    Memory backing;
    HardwareContext context;
    std::vector<InstructionWords> bytes;
    InstructionMapping mapping;
    uint64_t owner;
    std::unique_ptr<InstructionFetch> unit;
    FetchRig(bool rtl,uint32_t partitions=1,uint32_t words=1024,uint32_t capacity=64)
        : context(backing,configuration(rtl,capacity)),bytes(image(words)),mapping(context.instructions,bytes),owner(context.new_owner()),
          unit(make_instruction_fetch(context.memory,mapping,0,partitions,owner)) {}
    void tick() { context.tick(); unit->tick(); }
    InstructionWords fetch(uint32_t part,uint32_t pc) {
        for (uint32_t cycle=0; cycle<20000; ++cycle) {
            const auto words=unit->lookup(part,pc); tick(); if (words) return *words;
        }
        throw std::runtime_error("adapter fetch deadline exceeded");
    }
};

// A cold instruction line uses two real DRAM sectors; its other words are supplied by L2 hits.
// 冷指令行使用两个真实 DRAM sector，其余机器字由 L2 命中提供。
void clocks_and_true_cached_bytes(bool rtl) {
    FetchRig f(rtl); require(f.fetch(0,1)==f.bytes[1],"cold instruction words differ");
    const auto cold=f.unit->stats().clocks;
    require(cold==f.context.cycle() && f.unit->stats().memory_requests==4 && f.unit->stats().memory_responses==4,
        "refills or explicit hierarchy clocks differ");
    require(f.context.memory.dram_stats().reads==2 && f.context.memory.l1_stats(0).reads==0,"instruction traffic bypassed L2 or entered data L1");
    const auto original=f.bytes[1]; f.bytes[1][2]^=1234;
    require(f.fetch(0,1)==original && f.unit->stats().clocks==cold+1,"held instruction changed or added hidden clocks");
    require(f.fetch(0,2)==f.bytes[2] && f.unit->stats().clocks==cold+3,"synchronous cache hit timing differs");
    require(f.unit->stats().hits[0]==1 && f.unit->stats().misses[0]==1,"cache events differ");
    static_cast<void>(f.fetch(0,257));
    require(f.fetch(0,1)==original && f.unit->stats().memory_requests==12,"eviction did not reload immutable mapped bytes");
    require(f.context.memory.dram_stats().reads==4,"cached L2 instruction sectors reread DRAM");
}

void all_partitions_and_finite_memory_channel(bool rtl) {
    FetchRig f(rtl,8); uint32_t complete=0;
    for (uint32_t cycle=0; cycle<20000 && complete!=255; ++cycle) {
        for (uint32_t part=0; part<8; ++part) if (!(complete&(1u<<part))) {
            const auto result=f.unit->lookup(part,part*4+3);
            if (result) { require(*result==f.bytes[part*4+3],"partition received another refill"); complete|=1u<<part; }
        }
        f.tick();
    }
    require(complete==255 && !f.unit->busy(),"refill channel starved or retained completed work");
    const auto& stats=f.unit->stats();
    require(stats.memory_requests==32 && stats.memory_responses==32 && f.context.memory.dram_stats().reads==16,"physical requests bypassed or duplicated");
    for (uint32_t part=0; part<8; ++part)
        require(stats.misses[part]==1 && stats.fills[part]==4 && stats.delivered[part]==1 &&
            stats.queries[part]==stats.delivered[part]+stats.stalls[part],"partition accounting differs");
}

void input_phases_bounds_and_partial_line(bool rtl) {
    FetchRig f(rtl,1,3);
    require(!f.unit->lookup(0,0) && !f.context.cycle(),"initial query bypassed memory");
    rejects([&]{f.unit->lookup(0,1);},"instruction_fetch_already_queried");
    rejects([&]{f.unit->lookup(1,0);},"instruction_fetch_partition"); f.tick();
    require(f.fetch(0,2)==f.bytes[2],"partial line dropped valid word");
    rejects([&]{f.unit->lookup(0,3);},"pc_out_of_range");
    require(f.unit->stats().address_errors[0]==1,"address error did not originate at fetch port"); f.tick();
    require(f.fetch(0,2)==f.bytes[2],"invalid address corrupted cache");
    rejects([&]{make_instruction_fetch(f.context.memory,f.mapping,0,9,f.owner);},"invalid_instruction_fetch_configuration");
    rejects([&]{make_instruction_fetch(f.context.memory,f.mapping,2,1,f.owner);},"invalid_sm_index");
}

void query_isolation_and_abandoned_work(bool rtl) {
    FetchRig f(rtl); static_cast<void>(f.fetch(0,1));
    auto b=make_instruction_fetch(f.context.memory,f.mapping,1,1,f.owner);
    require(!b->lookup(0,1),"independent SM bypassed instruction cache lookup"); f.context.tick(); b->tick();
    const auto before=f.context.cycle(), first=f.unit->stats().clocks;
    for (uint32_t query=0; query<100; ++query)
        require(b->busy() && b->stats().clocks==1 && f.context.cycle()==before,"observation advanced memory");
    for (uint32_t cycle=0; cycle<2000 && b->busy(); ++cycle) { f.context.tick(); b->tick(); }
    require(!b->busy() && b->stats().memory_responses==4,"abandoned refill never drained");
    require(f.unit->stats().clocks==first && f.context.memory.dram_stats().reads==2,"shared L2 or independent SM clocks differ");
}

// An unconsumed data response retains the same hierarchy slot needed by instruction fetch.
// 未消费的数据响应保留取指也需要使用的同一个层次请求槽。
void shared_capacity_and_failure_drain(bool rtl) {
    FetchRig f(rtl,1,8,1); WarpAccess access; access.mask=1;
    const auto other=f.context.new_owner(); const auto data=accepted(f.context.memory.submit(1,access,other,true));
    for (uint32_t cycle=0; cycle<200; ++cycle) { static_cast<void>(f.unit->lookup(0,0)); f.tick(); }
    require(f.unit->stats().memory_stalls>0 && f.unit->stats().memory_requests==0,"fetch ignored occupied data response capacity");
    require(f.context.memory.take(data).has_value(),"data response was lost during fetch stall");
    require(f.fetch(0,0)==f.bytes[0],"fetch did not recover after capacity release");
    static_cast<void>(f.unit->lookup(0,4)); f.tick();
    for (uint32_t cycle=0; cycle<100 && f.unit->stats().memory_requests==4; ++cycle) f.tick();
    require(f.unit->stats().memory_requests==5 && !f.context.memory.drain_owner(f.owner),"no inflight instruction to drain");
    for (uint32_t cycle=0; cycle<2000 && !f.context.memory.drain_owner(f.owner); ++cycle) f.context.tick();
    require(f.context.memory.drain_owner(f.owner),"failed owner's instruction request did not drain");
    require(f.unit->stats().memory_requests==5,"fault draining issued younger instruction words");
}

void run_backend(bool rtl) {
    clocks_and_true_cached_bytes(rtl); std::cout<<"PASS fetch adapter real hierarchy clocks and immutable bytes "<<rtl<<'\n';
    all_partitions_and_finite_memory_channel(rtl); std::cout<<"PASS fetch adapter eight partitions and shared channel "<<rtl<<'\n';
    input_phases_bounds_and_partial_line(rtl); std::cout<<"PASS fetch adapter bounds and partial instruction line "<<rtl<<'\n';
    query_isolation_and_abandoned_work(rtl); std::cout<<"PASS fetch adapter shared L2, query isolation and drain "<<rtl<<'\n';
    shared_capacity_and_failure_drain(rtl); std::cout<<"PASS fetch adapter data contention and fault drain "<<rtl<<'\n';
}
void run_cases() { run_backend(false); if (cache_rtl_available()) run_backend(true); }
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return simt::hw::test::run_application(); }

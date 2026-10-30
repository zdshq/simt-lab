#include "simt/instruction_fetch.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,const char* expected) {
    bool failed=false;
    try { action(); } catch (const std::exception& e) { failed=std::string(e.what())==expected; }
    require(failed,"wrong or missing fetch error");
}
std::vector<InstructionWords> image(uint32_t words=1024) {
    std::vector<InstructionWords> result(words);
    for (uint32_t pc=0; pc<words; ++pc) result[pc]={pc,pc^0xdeadbeefu,pc*7,0x80000000u};
    return result;
}
InstructionWords fetch(InstructionFetch& unit,uint32_t part,uint32_t pc) {
    for (uint32_t cycle=0; cycle<2000; ++cycle) {
        const auto words=unit.lookup(part,pc); unit.tick(); if (words) return *words;
    }
    throw std::runtime_error("adapter fetch deadline exceeded");
}

void clocks_and_true_cached_bytes() {
    auto bytes=image(); auto unit=make_instruction_fetch(bytes,1);
    require(fetch(*unit,0,1)==bytes[1],"cold instruction words differ");
    require(unit->stats().clocks==40 && unit->stats().memory_requests==4 && unit->stats().memory_responses==4,
        "explicit eight-cycle memory/refill timing differs");
    const auto original=bytes[1]; bytes[1][2]^=1234;
    require(fetch(*unit,0,1)==original && unit->stats().clocks==41,"held instruction reread host image");
    require(fetch(*unit,0,2)==bytes[2] && unit->stats().clocks==43,"synchronous cache hit added hidden clocks");
    require(unit->stats().hits[0]==1 && unit->stats().misses[0]==1,"cache event observation differs");
    static_cast<void>(fetch(*unit,0,257));
    require(fetch(*unit,0,1)==bytes[1] && unit->stats().memory_requests==12,"tag replacement did not refill real data");
}

void all_partitions_and_finite_memory_channel() {
    auto bytes=image(); auto unit=make_instruction_fetch(bytes,8); uint32_t complete=0;
    for (uint32_t cycle=0; cycle<1000 && complete!=255; ++cycle) {
        for (uint32_t part=0; part<8; ++part) if (!(complete&(1u<<part))) {
            const auto result=unit->lookup(part,part*4+3);
            if (result) { require(*result==bytes[part*4+3],"partition received another refill"); complete|=1u<<part; }
        }
        unit->tick();
    }
    require(complete==255 && !unit->busy(),"shared refill channel starved or retained completed work");
    const auto& stats=unit->stats();
    require(stats.memory_requests==32 && stats.memory_responses==32,"physical partition requests were bypassed or duplicated");
    for (uint32_t part=0; part<8; ++part)
        require(stats.misses[part]==1 && stats.fills[part]==4 && stats.delivered[part]==1 &&
            stats.queries[part]==stats.delivered[part]+stats.stalls[part],"partition event accounting differs");
}

void input_phases_bounds_and_partial_line() {
    auto bytes=image(3); auto unit=make_instruction_fetch(bytes,1);
    require(!unit->lookup(0,0) && !unit->stats().clocks,"initial query bypassed memory");
    rejects([&]{unit->lookup(0,1);},"instruction_fetch_already_queried");
    rejects([&]{unit->lookup(1,0);},"instruction_fetch_partition"); unit->tick();
    require(fetch(*unit,0,2)==bytes[2],"partial line dropped valid word");
    rejects([&]{unit->lookup(0,3);},"pc_out_of_range");
    require(unit->stats().address_errors[0]==1,"address error did not originate at the fetch port"); unit->tick();
    require(fetch(*unit,0,2)==bytes[2],"invalid address corrupted cached word");
    rejects([&]{make_instruction_fetch({},1);},"invalid_instruction_fetch_configuration");
    rejects([&]{make_instruction_fetch(bytes,9);},"invalid_instruction_fetch_configuration");
}

void query_isolation_and_abandoned_work() {
    auto bytes=image(); auto a=make_instruction_fetch(bytes,1),b=make_instruction_fetch(bytes,1);
    static_cast<void>(fetch(*a,0,1)); require(!b->lookup(0,1),"independent SM shared a cache without a refill");
    b->tick(); const auto before=b->stats().clocks;
    for (uint32_t query=0; query<100; ++query) {
        require(b->busy() && b->stats().clocks==before,"observation advanced waiting memory");
        static_cast<void>(a->stats());
    }
    for (uint32_t cycle=0; cycle<100 && b->busy(); ++cycle) b->tick();
    require(!b->busy() && b->stats().memory_responses==4,"abandoned candidate leaked an outstanding refill");
    require(a->stats().clocks==40 && fetch(*b,0,1)==bytes[1],"SM instance clocks or data are not independent");
}

void run_cases() {
    clocks_and_true_cached_bytes(); std::cout<<"PASS fetch adapter clocks, cache bytes and tag replacement\n";
    all_partitions_and_finite_memory_channel(); std::cout<<"PASS fetch adapter eight partitions and finite backing channel\n";
    input_phases_bounds_and_partial_line(); std::cout<<"PASS fetch adapter input phases, bounds and partial line\n";
    query_isolation_and_abandoned_work(); std::cout<<"PASS fetch adapter query isolation and abandoned work\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

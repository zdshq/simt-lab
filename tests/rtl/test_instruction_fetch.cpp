#include "Vinstruction_fetch.h"
#include "bench.hpp"
#include <array>
#include <optional>
#include <random>
#include <vector>

namespace {
using Words=std::array<uint32_t,4>;
Words payload(uint32_t pc,uint32_t epoch=0) { return {pc,pc^0xa55aa55au,pc*3+epoch,0x80000000u+epoch}; }

class FetchBench {
public:
    Bench<Vinstruction_fetch> b{nullptr};
    std::vector<uint32_t> requests;
    uint32_t epoch=0, latency=3;
    FetchBench() { b.dut.partition_limit=8; b.dut.program_words=uint64_t{1}<<32; b.dut.eval(); }
    void select(uint32_t part,uint32_t pc) { b.dut.query_valid|=1u<<part; b.dut.query_pcs[part]=pc; b.dut.eval(); }
    bool ready(uint32_t part) const { return b.dut.response_valid&(1u<<part); }
    Words words(uint32_t part) const {
        Words result; for (uint32_t word=0; word<4; ++word) result[word]=b.dut.response_words[part*4+word]; return result;
    }
    void step(bool accept=true,bool deliver=true);
    Words fetch(uint32_t part,uint32_t pc);
    void reset();
private:
    struct Pending { uint64_t due; Words words; };
    std::optional<Pending> pending_;
};

// This independent memory source can pause requests and responses without advancing the DUT secretly.
// 独立存储源可以暂停请求与响应，不暗中推进被测电路。
void FetchBench::step(bool accept,bool deliver) {
    auto& d=b.dut; d.memory_request_ready=accept && !pending_;
    d.memory_response_valid=deliver && pending_ && pending_->due<=b.cycle;
    if (d.memory_response_valid) for (uint32_t word=0; word<4; ++word) d.memory_response_words[word]=pending_->words[word];
    d.eval();
    const bool request=d.memory_request_valid && d.memory_request_ready;
    const bool response=d.memory_response_valid && d.memory_response_ready;
    if (response) pending_.reset();
    if (request) {
        require(!pending_,"refill channel accepted two outstanding reads");
        requests.push_back(d.memory_request_pc);
        pending_=Pending{b.cycle+latency,payload(d.memory_request_pc,epoch)};
    }
    b.tick(); d.memory_request_ready=0; d.memory_response_valid=0; d.eval();
}
Words FetchBench::fetch(uint32_t part,uint32_t pc) {
    select(part,pc);
    for (uint32_t attempt=0; attempt<2000; ++attempt) {
        if (ready(part)) return words(part);
        step(b.cycle%4!=0,b.cycle%5!=0);
    }
    throw std::runtime_error("instruction fetch made no progress");
}
void FetchBench::reset() {
    pending_.reset(); b.dut.query_valid=0; b.dut.memory_response_valid=0; b.dut.reset=1;
    b.tick(); b.dut.reset=0; b.dut.eval();
}

template<class Predicate> void advance_until(FetchBench& f,Predicate done) {
    for (uint32_t cycle=0; cycle<2000; ++cycle) { if (done()) return; f.step(); }
    throw std::runtime_error("refill progress deadline exceeded");
}

void synchronous_hits_and_real_cached_words() {
    FetchBench f;
    require(f.fetch(0,1)==payload(1) && f.requests==std::vector<uint32_t>({0,1,2,3}),"cold line refill differs");
    require(f.b.dut.busy,"new response disappeared from pending work before consumption");
    f.step(); require(!f.b.dut.busy,"consumed response remained pending work");
    f.epoch=91; f.select(0,2);
    require(!f.ready(0),"new word lookup bypassed synchronous memory"); f.step();
    require(f.ready(0) && f.words(0)==payload(2),"hit reread external bytes or selected wrong word");
    for (uint32_t hold=0; hold<20; ++hold) { f.step(); require(f.words(0)==payload(2),"held result changed"); }
    require(f.requests.size()==4,"cache hits generated external traffic");
    require(f.fetch(0,257)==payload(257,91),"alias tag did not refill");
    require(f.fetch(0,1)==payload(1,91) && f.requests.size()==12,"evicted line did not reload actual bytes");
}

void eight_partition_round_robin_and_backpressure() {
    FetchBench f;
    for (uint32_t part=0; part<8; ++part) f.select(part,part*4);
    for (uint32_t cycle=0; cycle<8; ++cycle) f.step(false,false);
    require(f.b.dut.memory_request_valid && f.b.dut.memory_request_pc==0,"initial refill owner not retained");
    for (uint32_t cycle=0; cycle<20; ++cycle) {
        f.step(false,false); require(f.b.dut.memory_request_pc==0 && !f.b.dut.response_valid,"request backpressure changed address/result");
    }
    for (uint32_t cycle=0; cycle<2000 && f.b.dut.response_valid!=255; ++cycle) f.step(cycle%3!=0,cycle%4!=0);
    require(f.b.dut.response_valid==255 && f.requests.size()==32,"all physical partition misses did not complete");
    for (uint32_t word=0; word<32; ++word) require(f.requests[word]==word,"round-robin line ownership/interleaving differs");
    for (uint32_t part=0; part<8; ++part) require(f.words(part)==payload(part*4),"refill delivered to wrong cache");
}

void changed_candidates_and_inflight_reset() {
    FetchBench f; f.select(7,3);
    advance_until(f,[&]{return f.requests.size()>=2;});
    f.select(7,259);
    advance_until(f,[&]{return f.ready(7);});
    require(f.words(7)==payload(259),"old candidate leaked its instruction");
    require(f.requests.size()==8,"changed candidate lost either refill");
    f.select(7,515);
    advance_until(f,[&]{return f.requests.size()>=10;});
    f.reset(); f.epoch=7; const auto before=f.requests.size();
    require(!f.b.dut.busy && !f.b.dut.response_valid,"reset retained outstanding ownership");
    require(f.fetch(7,515)==payload(515,7) && f.requests.size()==before+4,"partially filled line survived reset validity");
    f.select(7,771); f.step(); f.b.dut.query_valid=0; f.b.dut.eval();
    for (uint32_t cycle=0; cycle<200 && f.b.dut.busy; ++cycle) f.step();
    require(!f.b.dut.busy && !f.b.dut.response_valid,"abandoned lookup never drained");
}

void full_pc_width_and_partial_last_line() {
    FetchBench f;
    require(f.fetch(7,0xffffffff)==payload(0xffffffff),"highest PC was truncated");
    require(f.requests.back()==0xffffffff && f.requests.front()==0xfffffffc,"highest line wrapped address");
    f.reset(); f.b.dut.program_words=3;
    require(f.fetch(0,2)==payload(2),"partial last line rejected legal instruction");
    f.select(0,3); require((f.b.dut.address_errors&1) && !f.ready(0),"padding became an executable instruction");
    f.reset(); f.b.dut.partition_limit=1; f.select(7,0); f.step();
    require(!f.b.dut.response_valid && !f.b.dut.busy,"disabled partition accessed storage");
    f.b.dut.partition_limit=9; f.b.dut.eval(); require(f.b.dut.configuration_error,"invalid partition geometry accepted");
}

void randomized_replacement_and_all_words() {
    FetchBench f; std::mt19937 rng(826041);
    for (uint32_t sample=0; sample<400; ++sample) {
        const uint32_t part=rng()%8,pc=(rng()%1024)|((sample%17==0) ? 0x80000000u : 0u);
        require(f.fetch(part,pc)==payload(pc),"random bank/tag/offset replacement differs");
    }
    for (uint32_t part=0; part<8; ++part)
        for (uint32_t word=0; word<256; ++word) require(f.fetch(part,word)==payload(word),"full cache capacity or word offset differs");
}

void run_cases() {
    run_case("instruction fetch synchronous hit, actual cached bytes and tag eviction",synchronous_hits_and_real_cached_words);
    run_case("instruction fetch all eight partitions, refill fairness and backpressure",eight_partition_round_robin_and_backpressure);
    run_case("instruction fetch changed candidates, abandoned work and inflight reset",changed_candidates_and_inflight_reset);
    run_case("instruction fetch full PC width, partial line and disabled partitions",full_pc_width_and_partial_last_line);
    run_case("instruction fetch 400 random replacements and every physical cache word",randomized_replacement_and_all_words);
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

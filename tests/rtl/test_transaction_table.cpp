#include "Vtransaction_table.h"
#include "bench.hpp"
#include "transaction_reference.hpp"
#include <map>
#include <random>
#include <vector>

namespace {
using TestBench=Bench<Vtransaction_table>;
using transaction_reference::Record;
using Key=std::pair<uint32_t,uint32_t>;
struct Oracle { uint32_t capacity; std::map<Key,Record> records; };
struct Command { uint32_t op,cta,address=0,amount=0,phase=0; };

transaction_reference::Result reference(Oracle& model,Command c) {
    if (c.op==6) {
        for (const auto& [key,r] : model.records) if (key.first==c.cta && (r[3] || r[4])) return {{},14};
        std::erase_if(model.records,[&](const auto& item){return item.first.first==c.cta;}); return {};
    }
    const Key key{c.cta,c.address};
    if (c.op==0) {
        if (!c.amount || c.amount>1024) return {{},3};
        if (c.address%16 || c.address>65504) return {{},15};
        for (const auto& [existing,r] : model.records) {
            static_cast<void>(r);
            if (existing.first==c.cta && uint64_t{existing.second}<uint64_t{c.address}+32 &&
                uint64_t{c.address}<uint64_t{existing.second}+32) return {{},4};
        }
        if (model.records.size()==model.capacity) return {{},5};
    } else if (!model.records.contains(key)) return {{},6};
    if (c.op==5) return {model.records.at(key)};
    auto next=transaction_reference::update(c.op==0 ? Record{} : model.records.at(key),c.op,c.amount,c.phase);
    if (!next.error) model.records[key]=next.record;
    return next;
}
void compare_record(const TestBench& b,const Record& r) {
    for (uint32_t i=0; i<5; ++i) require(b.dut.result_record[4-i]==r[i],"transaction RAM output differs");
}
void submit(TestBench& b,Command c,uint32_t capacity) {
    require(b.dut.command_ready,"table not ready");
    b.dut.command_valid=1; b.dut.operation=c.op; b.dut.capacity=capacity; b.dut.cta=c.cta;
    b.dut.address=c.address; b.dut.amount=c.amount; b.dut.completion_phase=c.phase; b.tick();
    b.dut.command_valid=0; b.dut.eval();
}

// Publication and response backpressure cannot expose early state or accept another command.
// 发布和响应反压不能提前暴露状态或接收另一条命令。
void run(TestBench& b,Oracle& model,Command c,uint32_t stall=0) {
    auto next=model; const auto old_size=model.records.size(); const auto expected=reference(next,c);
    const auto token=c.op==0 || c.op==6 || !model.records.contains({c.cta,c.address}) ? 0 : model.records.at({c.cta,c.address})[0];
    submit(b,c,model.capacity); uint32_t clocks=0;
    while (!b.dut.publication_valid && !b.dut.response_valid) { require(++clocks<model.capacity*5+10,"table search hung"); b.tick(); }
    if (b.dut.publication_valid) {
        require(!expected.error && c.op<5,"unexpected publication"); compare_record(b,expected.record);
        for (uint32_t i=0; i<stall; ++i) { b.tick(); compare_record(b,expected.record); require(!b.dut.command_ready && !b.dut.response_valid && b.dut.occupancy==old_size,"publication leaked early"); }
        b.dut.publication_ack=1; b.tick(); b.dut.publication_ack=0; b.dut.eval();
    }
    require(b.dut.response_valid && b.dut.error==expected.error,"table response differs");
    require(b.dut.result_cta==c.cta && b.dut.result_address==c.address,"response owner lost");
    if (!expected.error && c.op!=6) {
        compare_record(b,expected.record);
        require(b.dut.arrival_token==token && bool(b.dut.phase_advanced)==expected.advanced,"phase event/token differs");
    }
    for (uint32_t i=0; i<stall; ++i) { b.tick(); require(b.dut.response_valid && !b.dut.command_ready,"response not held"); }
    b.dut.response_ready=1; b.tick(); b.dut.response_ready=0; b.dut.eval(); model=std::move(next);
    require(b.dut.command_ready && b.dut.occupancy==model.records.size(),"table occupancy or readiness differs");
}
void publication_tokens_and_phases() {
    TestBench b("transaction-publication.vcd"); Oracle r{16,{}};
    run(b,r,{0,3,1024,1},12); run(b,r,{1,3,1024,32}); run(b,r,{2,3,1024},7);
    run(b,r,{3,3,1024,12}); run(b,r,{3,3,1024,20}); run(b,r,{4,3,1024,12,0},5);
    run(b,r,{5,3,1024}); run(b,r,{4,3,1024,20,0},11); require(r.records.at({3,1024})[0]==1,"phase did not complete");
    run(b,r,{2,3,1024},3); run(b,r,{6,3});
}
void full_capacity_aliasing_and_reuse() {
    TestBench b(nullptr); Oracle r{TRANSACTION_SLOTS,{}};
    for (uint32_t i=0; i<TRANSACTION_SLOTS; ++i) run(b,r,{0,i,65504,1});
    run(b,r,{0,UINT32_MAX,65504,1});
    run(b,r,{2,TRANSACTION_SLOTS-1,65504}); run(b,r,{5,0,65504});
    run(b,r,{6,TRANSACTION_SLOTS-1}); run(b,r,{0,UINT32_MAX,65504,1024}); run(b,r,{5,UINT32_MAX,65504});
    require(r.records.at({UINT32_MAX,65504})==Record{0,1024,1024,0,0},"slot reuse inherited state");
    b.dut.reset=1; b.tick(); b.dut.reset=0; b.dut.eval(); r.records.clear(); run(b,r,{5,0,65504});
}
void rejection_and_retirement_atomicity() {
    TestBench b("transaction-errors.vcd"); Oracle r{16,{}};
    run(b,r,{0,0,0,0}); run(b,r,{0,0,8,1}); run(b,r,{0,0,65520,1});
    run(b,r,{0,0,0,1}); run(b,r,{0,0,16,1}); run(b,r,{0,0,32,1});
    run(b,r,{1,0,32,UINT32_MAX}); run(b,r,{1,0,32,1}); run(b,r,{2,0,32}); run(b,r,{2,0,32});
    run(b,r,{3,0,32,0}); run(b,r,{3,0,32,4097}); run(b,r,{3,0,32,1});
    run(b,r,{4,0,32,1,1}); run(b,r,{4,0,32,2,0}); run(b,r,{6,0});
    run(b,r,{5,0,0}); run(b,r,{5,0,32}); run(b,r,{5,1,32});
    submit(b,{5,0},0); require(b.dut.response_valid && b.dut.error==2,"invalid capacity accepted");
}
void random_commands_and_records() {
    TestBench b(nullptr); Oracle r{16,{}}; std::mt19937 rng(457903);
    for (uint32_t i=0; i<2400; ++i) {
        Command c{uint32_t(rng()%7),uint32_t(rng()%4),uint32_t((rng()%8)*32),uint32_t(1+rng()%8),0};
        if (r.records.contains({c.cta,c.address})) c.phase=r.records.at({c.cta,c.address})[0];
        run(b,r,c,i%73==0 ? 4 : 0);
    }
    std::vector<Key> keys;
    for (const auto& [key,value] : r.records) { static_cast<void>(value); keys.push_back(key); }
    for (const auto& key : keys) run(b,r,{5,key.first,key.second});
}
int run_application() {
    try { run_case("transaction table publication/response backpressure, bytes and phase tokens",publication_tokens_and_phases);
        run_case("transaction table full physical capacity, CTA aliases, highest slot and reset",full_capacity_aliasing_and_reuse);
        run_case("transaction table rejected arithmetic, overlap and atomic retirement",rejection_and_retirement_atomicity);
        run_case("transaction table 2400 randomized commands and independent records",random_commands_and_records); return 0; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

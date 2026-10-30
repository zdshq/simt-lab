#include "Vasync_service.h"
#include "bench.hpp"
#include <algorithm>
#include <deque>
#include <random>

namespace {
struct Driver {
    Bench<Vasync_service> b{nullptr};
    Driver(uint32_t latency=1) { b.dut.cta_limit=64; b.dut.latency=latency; b.dut.eval(); }
    void edge() {
        b.tick(); auto& d=b.dut;
        d.accept=0; d.complete=0; d.retry=0; d.retire_ack=0; d.dma_valid=0; d.retire_requests=0; d.eval();
    }
    void accept() { b.dut.accept=1; b.dut.eval(); require(!b.dut.error,"legal acceptance rejected"); }
    void finish() { b.dut.complete=1; b.dut.eval(); require(!b.dut.error,"legal completion rejected"); }
};

void priority_and_retirement() {
    Driver t; auto& d=t.b.dut; d.retire_requests=UINT64_MAX; d.table_ready=1;
    d.dma_valid=1; d.dma_id=UINT64_MAX; d.dma_cta=UINT32_MAX; d.eval();
    require(d.grant_kind==2,"DMA did not precede retirement"); t.accept(); t.edge();
    require(d.owner_id==UINT64_MAX && d.owner_cta==UINT32_MAX && d.service_ready,"DMA fields truncated");
    t.finish(); t.edge();
    for (uint32_t n=0; n<128; ++n) {
        d.retire_requests=UINT64_MAX; d.retire_ctas[n%64]=UINT32_MAX-n; d.eval();
        require(d.grant_kind==3 && d.grant_slot==n%64 && !d.foreground_ready,"retirement rotation differs");
        t.accept(); t.edge(); require(d.owner_cta==UINT32_MAX-n,"CTA identity truncated");
        t.finish(); t.edge();
        for (uint32_t hold=0; hold<3; ++hold) {
            require(d.retirement_done && d.owner_kind==3 && !d.grant_kind,"retirement lost before acknowledgement"); t.edge();
        }
        d.retire_ack=1; d.eval(); require(!d.error && !d.busy,"retirement acknowledgement failed"); t.edge();
    }
    d.retire_requests=uint64_t{1}<<63; d.table_ready=0; d.eval();
    require(!d.grant_kind && d.pending,"table backpressure lost pending release");
    d.dma_valid=1; d.eval(); require(d.grant_kind==2,"DMA reservation incorrectly needs a ready table");
}

void latency_retry_and_handoff() {
    for (uint32_t latency : {1u,3u,17u}) {
        Driver t(latency); auto& d=t.b.dut;
        d.instruction_id=UINT64_MAX-7; d.instruction_cta=79; t.accept();
        require(!d.service_ready && d.remaining==latency-1,"accepted work serviced before edge"); t.edge();
        for (uint32_t cycle=1; cycle<latency; ++cycle) { require(!d.service_ready,"early ready"); t.edge(); }
        require(d.service_ready && d.owner_id==UINT64_MAX-7,"deadline or owner wrong");
        d.retry=1; d.eval(); require(!d.error && !d.service_ready,"retry not rearmed"); t.edge();
        for (uint32_t cycle=1; cycle<latency; ++cycle) t.edge();
        require(d.service_ready,"retry deadline missed"); t.finish();
        d.dma_valid=1; d.dma_id=42; d.eval(); t.accept();
        require(d.accepted_kind==2 && d.completed_kind==1,"same-cycle handoff event identities wrong"); t.edge();
        require(d.owner_kind==2 && d.owner_id==42,"handoff owner lost");
    }
    Driver t(UINT32_MAX); auto& d=t.b.dut; t.accept(); t.edge();
    require(d.remaining==UINT32_MAX-1,"32-bit latency truncated");
    for (uint32_t i=0; i<9; ++i) t.edge();
    require(d.remaining==UINT32_MAX-10,"full-width counter failed");
}

void invalid_batches_and_reset() {
    Driver t(5); auto& d=t.b.dut; t.accept(); t.edge();
    d.complete=1; d.eval(); require(d.error==3,"early completion accepted"); t.edge();
    require(d.owner_kind==1 && d.remaining==4,"invalid batch changed owner or timer");
    d.retire_ack=1; d.eval(); require(d.error==5,"foreign retirement acknowledgement accepted"); t.edge();
    d.cta_limit=63; d.retire_requests=uint64_t{1}<<63; d.eval(); require(d.error==2,"disabled CTA slot accepted"); t.edge();
    d.latency=0; d.eval(); require(d.error==1,"zero latency accepted"); t.edge();
    d.reset=1; t.edge(); d.reset=0; d.latency=1; d.eval();
    require(!d.busy && !d.remaining && !d.cursor && !d.clock_enable,"reset left live state");
    d.retry=1; d.eval(); require(d.error==4,"idle retry accepted"); t.edge();
    d.complete=1; d.retry=1; d.eval(); require(d.error,"invalid simultaneous events accepted"); t.edge();
}

// A queue models fairness and absolute timestamps model delay, independently of RTL counters.
// 用队列建模公平性、绝对时间戳建模延迟，不复制 RTL 计数器。
void random_jobs_and_deadlines() {
    Driver t; auto& d=t.b.dut; std::mt19937_64 rng(83051); std::deque<uint32_t> order;
    for (uint32_t slot=0; slot<64; ++slot) order.push_back(slot);
    for (uint32_t job=0; job<2000; ++job) {
        d.latency=1+rng()%13; d.table_ready=1; d.retire_requests=rng(); d.dma_valid=rng()%3==0;
        d.dma_id=rng(); d.dma_cta=rng(); d.instruction_id=rng(); d.instruction_cta=rng();
        if (job%3==0) d.retire_requests=0;
        for (auto& cta : d.retire_ctas) cta=rng();
        const uint32_t kind=d.dma_valid ? 2 : d.retire_requests ? 3 : 1;
        auto at=std::find_if(order.begin(),order.end(),[&](auto slot){return d.retire_requests & (uint64_t{1}<<slot);});
        const uint32_t slot=kind==3 ? *at : 0;
        const uint64_t id=kind==2 ? d.dma_id : kind==1 ? d.instruction_id : 0;
        d.eval(); require(d.grant_kind==kind && (kind!=3 || d.grant_slot==slot),"random source selection differs");
        if (job%5==0) { t.b.tick(); require(d.grant_kind==kind && !d.busy,"unaccepted offer acquired owner"); }
        const auto deadline=t.b.cycle+d.latency; t.accept(); t.edge();
        require(d.owner_kind==kind && d.owner_id==id,"random owner identity differs");
        if (kind==3) {
            std::rotate(order.begin(),std::next(at)==order.end() ? order.begin() : std::next(at),order.end());
            require(d.cursor==order.front(),"random fairness cursor differs");
        } else {
            while (t.b.cycle<deadline) { require(!d.service_ready,"random early deadline"); t.edge(); }
            require(d.service_ready,"random late deadline");
        }
        t.finish();
        if (kind==3) { d.retire_ack=1; d.eval(); require(!d.error,"complete and acknowledge rejected"); }
        t.edge(); require(!d.busy,"random job not released");
    }
}
int run_application() {
    try {
        run_case("async priority, full 64-slot retirement rotation and acknowledgement",priority_and_retirement);
        run_case("async latency, retry, full-width counter and same-cycle handoff",latency_retry_and_handoff);
        run_case("async invalid batch atomicity, limits and in-flight reset",invalid_batches_and_reset);
        run_case("async 2000 randomized jobs with queue fairness and absolute deadlines",random_jobs_and_deadlines); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

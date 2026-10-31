#include "Vcommand_processor.h"
#include "bench.hpp"
#include <random>
#include <vector>

namespace {
constexpr uint32_t reg_put=0, reg_get=1, reg_notified=2, reg_status=3, reg_capacity=4;
constexpr uint32_t compute=0, copy=1, no_engine=2;

// The driver holds pins until each edge, matching how the device adapter drives the circuit.
// 驱动器在每个边沿之前保持引脚，与设备适配器驱动电路的方式一致。
struct Driver {
    Bench<Vcommand_processor> b{nullptr};
    explicit Driver(uint32_t capacity=64) { b.dut.capacity=capacity; clear(); }
    void clear() {
        auto& d=b.dut;
        d.mmio_valid=0; d.mmio_write=0; d.mmio_address=0; d.fetch_accept=0; d.fetch_wait=0; d.external_pending=0;
        d.external_failed=0; d.node_valid=0; d.node_cancel=0; d.graph_pending=0; d.complete_mask=0; d.failed_mask=0;
        d.compute_release=0; d.copy_release=0; d.eval();
    }
    void tick() {
        require(!b.dut.error,"unexpected CP protocol error "+std::to_string(b.dut.error));
        b.tick(); clear();
    }
    uint32_t read(uint32_t address) { b.dut.mmio_address=address; b.dut.eval(); return b.dut.mmio_rdata; }
    void doorbell(uint32_t put) {
        auto& d=b.dut; d.mmio_valid=1; d.mmio_write=1; d.mmio_address=reg_put; d.mmio_wdata=put; d.eval();
    }
    uint32_t fetch(uint32_t handle,uint32_t kind,uint32_t wait=0) {
        auto& d=b.dut; d.eval(); require(d.fetch_valid,"fetch unavailable");
        const uint32_t slot=d.fetch_slot; d.fetch_accept=1; d.fetch_handle=handle; d.fetch_kind=kind; d.fetch_wait=wait; d.eval();
        tick(); return slot;
    }
    bool dispatched(uint32_t slot) { b.dut.eval(); return b.dut.dispatch_valid && !b.dut.dispatch_node && b.dut.dispatch_slot==slot; }
    void complete(uint32_t slot,bool failed=false) {
        b.dut.complete_mask|=1u<<slot; if (failed) b.dut.failed_mask|=1u<<slot; b.dut.eval();
    }
    uint32_t lookup(uint32_t handle) { b.dut.lookup_handle=handle; b.dut.eval(); return b.dut.lookup_mask; }
};

// Pointers follow publication order, and an unpublished ring entry is never visible.
// 指针遵循发布顺序，未发布的队列项不可见。
void doorbell_bypass_and_sixteen_slot_limit() {
    Driver t; auto& d=t.b.dut;
    d.eval(); require(!d.fetch_valid,"fetched before doorbell");
    t.doorbell(20); require(d.fetch_valid && t.read(reg_get)==0,"doorbell did not bypass");
    t.doorbell(20); require(t.read(reg_put)==20,"held doorbell invisible"); d.mmio_address=reg_put; d.eval(); t.tick();
    require(t.read(reg_put)==20 && t.read(reg_capacity)==64,"put register lost");
    for (uint32_t job=0; job<16; ++job) { require(t.fetch(job+1,no_engine)==job,"lowest free slot not chosen"); }
    d.eval(); require(!d.fetch_valid && t.read(reg_get)==16 && (t.read(reg_status)&31)==16,"seventeenth packet admitted");
    t.complete(0); d.eval(); require(!d.fetch_valid,"release visible before its edge"); t.tick();
    require(d.fetch_valid && t.read(reg_notified)==1,"release or notification missing");
}

// A dependency on a live slot blocks dispatch until the producer's completion edge.
// 对存活槽位的依赖会阻止派发，直到生产者的完成边沿。
void dependency_masks_failure_and_same_edge_fetch() {
    Driver t; auto& d=t.b.dut; t.doorbell(5); d.eval();
    const auto first=t.fetch(10,copy); require(t.lookup(10)==(1u<<first) && !t.lookup(99),"handle CAM wrong");
    require(d.copy_busy,"fetched producer did not start on its own clock");
    const auto second=t.fetch(11,copy,1u<<first);
    d.eval(); require(d.copy_busy && !d.dispatch_valid,"waiter dispatched early");
    d.copy_release=1; t.complete(first); d.eval(); t.tick();
    require(t.dispatched(second) && !d.dispatch_cancel,"waiter not released");
    d.copy_release=1; t.complete(second,true);
    d.fetch_accept=1; d.fetch_handle=12; d.fetch_kind=compute; d.fetch_wait=1u<<second; d.eval();
    const uint32_t third=d.fetch_slot; t.tick();
    require(t.dispatched(third) && d.dispatch_cancel,"same-edge failed producer lost");
    t.complete(third,true); d.eval(); t.tick();
    d.eval(); const uint32_t fourth=d.fetch_slot; d.external_failed=1u<<fourth;
    d.fetch_accept=1; d.fetch_handle=13; d.fetch_kind=no_engine; d.fetch_wait=0; d.eval();
    require(t.dispatched(fourth) && d.dispatch_cancel,"failed completion record ignored");
    t.complete(fourth,true); d.eval(); t.tick(); require(!(d.slot_valid&(1u<<fourth)),"same-clock fetch and completion kept the slot");
    d.eval(); const uint32_t fifth=d.fetch_slot; d.external_pending=1u<<fifth;
    d.fetch_accept=1; d.fetch_handle=14; d.fetch_kind=no_engine; d.fetch_wait=0; d.eval();
    require(!d.dispatch_valid,"pending record ignored"); t.tick();
    d.external_pending=0; d.eval(); require(t.dispatched(fifth),"cleared record blocked dispatch");
}

// Engines are exclusive, but a blocked packet does not block a younger ready one.
// 引擎互斥，但被阻塞的包不会阻塞更年轻的就绪包。
void engine_occupancy_and_graph_decoder() {
    Driver t; auto& d=t.b.dut; t.doorbell(6); d.eval();
    const auto a=t.fetch(20,compute); require(d.compute_busy,"compute not claimed");
    const auto b=t.fetch(21,compute); d.eval(); require(!d.dispatch_valid,"busy engine double claimed");
    t.fetch(22,copy); require(d.copy_busy,"compute wait blocked younger copy");
    t.fetch(23,no_engine); require(d.slot_running&(1u<<3),"engine-free packet blocked");
    d.compute_release=1; t.complete(a); d.eval(); t.tick();
    require(t.dispatched(b),"released engine unused");
    d.compute_release=1; d.eval(); t.tick(); require(!d.compute_busy,"grant and same-edge release retained busy");
    const auto g=t.fetch(24,no_engine), f=t.fetch(25,no_engine);
    d.graph_pending=(1u<<g)|(1u<<f); d.eval(); require(d.graph_valid && d.graph_slot==g,"oldest graph not chosen");
    d.graph_pending=1u<<f; d.eval(); require(d.graph_slot==f,"other graph not chosen"); d.graph_pending=0; d.eval();
    d.node_valid=1; d.node_kind=compute; d.node_handle=1; d.eval(); require(d.dispatch_node,"older node lost");
    t.tick(); require(d.compute_busy,"node did not claim engine");
    d.node_valid=1; d.node_cancel=1; d.node_kind=compute; d.node_handle=2; d.eval();
    require(d.dispatch_node && d.dispatch_cancel && !d.error,"cancelling node needed a free engine"); t.tick();
}

// A rejected batch is reported and leaves pointers, slots and engines unchanged.
// 被拒绝的批次会报告错误，并保持指针、槽位和引擎不变。
void invalid_batches_are_atomic() {
    Driver t; auto& d=t.b.dut;
    const auto rejected=[&](uint32_t code) { d.eval(); require(d.error==code,"wrong CP error "+std::to_string(d.error)); t.b.tick(); t.clear(); };
    t.doorbell(65); rejected(2); require(t.read(reg_put)==0,"over-capacity doorbell committed");
    d.mmio_valid=1; d.mmio_write=1; d.mmio_address=reg_get; rejected(2);
    d.fetch_accept=1; rejected(3); require(t.read(reg_get)==0,"fetch without packet committed");
    d.complete_mask=1; rejected(4); require(t.read(reg_notified)==0,"empty slot completion counted");
    d.compute_release=1; rejected(5); d.copy_release=1; rejected(5);
    d.graph_pending=1; rejected(7);
    t.doorbell(2); d.eval(); const auto a=t.fetch(30,compute); require(d.compute_busy,"packet missing");
    d.node_valid=1; d.node_kind=compute; d.node_handle=99; rejected(6);
    d.failed_mask=1u<<a; rejected(4); d.node_valid=1; d.node_kind=3; rejected(6);
    d.fetch_accept=1; d.fetch_kind=3; rejected(3); d.fetch_accept=1; d.fetch_wait=0x8000; rejected(3);
    d.capacity=0; rejected(1); d.capacity=4097; rejected(1); d.capacity=64; d.eval();
    require(d.compute_busy && t.read(reg_get)==1,"invalid batch changed state");
    d.reset=1; t.b.tick(); d.reset=0; t.clear(); require(!d.compute_busy && !d.slot_valid && !t.read(reg_put),"reset retained state");
}

// Compare random traffic with an independent list model of the documented selection rules.
// 用按文档规则独立实现的列表模型，对照随机流量的选择结果。
struct Packet { uint32_t handle,kind,slot; std::vector<uint32_t> waits; bool running=false,failed=false,claimed=false; };
const Packet* expected_oldest(const std::vector<Packet>& live,bool compute_busy,bool copy_busy) {
    const Packet* best=nullptr;
    for (const auto& p : live) {
        if (p.running) continue;
        const bool free=p.kind==no_engine || (p.kind==compute ? !compute_busy : !copy_busy);
        if ((p.failed || (p.waits.empty() && free)) && (!best || p.handle<best->handle)) best=&p;
    }
    return best;
}
void finish_random_packet(Driver& t,std::vector<Packet>& live,std::mt19937& rng) {
    std::vector<size_t> running;
    for (size_t at=0; at<live.size(); ++at) if (live[at].running) running.push_back(at);
    if (running.empty()) return;
    const auto index=running[rng()%running.size()]; const auto done=live[index]; const bool failed=rng()%8==0;
    t.complete(done.slot,failed);
    if (done.claimed && done.kind==compute) t.b.dut.compute_release=1;
    if (done.claimed && done.kind==copy) t.b.dut.copy_release=1;
    live.erase(live.begin()+static_cast<long>(index));
    for (auto& p : live) for (size_t at=0; at<p.waits.size(); ++at) if (p.waits[at]==done.handle) {
        p.waits.erase(p.waits.begin()+static_cast<long>(at)); p.failed|=failed; break;
    }
}
void admit_random_packet(Driver& t,std::vector<Packet>& live,std::mt19937& rng,uint32_t& handle) {
    auto& d=t.b.dut; d.eval();
    if (!d.fetch_valid || rng()%2) return;
    Packet p{handle++,rng()%3,d.fetch_slot,{}};
    uint32_t mask=0;
    for (const auto& older : live) if (!older.running && rng()%4==0) { mask|=1u<<older.slot; p.waits.push_back(older.handle); }
    d.fetch_accept=1; d.fetch_handle=p.handle; d.fetch_kind=p.kind; d.fetch_wait=mask; d.eval();
    live.push_back(p);
}
void random_traffic_matches_reference() {
    Driver t(4096); auto& d=t.b.dut; std::mt19937 rng(61127); std::vector<Packet> live; uint32_t handle=1,put=0,done=0;
    for (uint32_t clock=0; clock<40000; ++clock) {
        if (put-t.read(reg_get)<4096 && rng()%3==0) t.doorbell(++put);
        admit_random_packet(t,live,rng,handle);
        d.eval(); const auto* want=expected_oldest(live,d.compute_busy,d.copy_busy);
        require(bool(d.dispatch_valid)==bool(want),"dispatch presence differs from reference");
        if (want) {
            require(d.dispatch_slot==want->slot && bool(d.dispatch_cancel)==want->failed,"dispatch choice differs from reference");
            auto& chosen=const_cast<Packet&>(*want); chosen.running=true; chosen.claimed=!chosen.failed;
        }
        const auto before=live.size(); finish_random_packet(t,live,rng); done+=static_cast<uint32_t>(before-live.size());
        d.mmio_address=reg_put; t.tick();
        require(t.read(reg_notified)==done,"notification count differs from reference");
    }
    require(done>1000,"random traffic made too little progress");
}
int run_application() {
    try {
        run_case("CP doorbell bypass and sixteen decoded slots",doorbell_bypass_and_sixteen_slot_limit);
        run_case("CP dependency masks, failure propagation and completion records",dependency_masks_failure_and_same_edge_fetch);
        run_case("CP engine occupancy, oldest selection and graph decoder",engine_occupancy_and_graph_decoder);
        run_case("CP invalid batches are atomic and reset clears state",invalid_batches_are_atomic);
        run_case("CP 40000 random clocks match an independent reference",random_traffic_matches_reference); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

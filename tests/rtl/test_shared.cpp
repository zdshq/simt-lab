#include "Vshared_memory.h"
#include "bench.hpp"
#include <array>
#include <bit>
#include <random>
#include <vector>

namespace {
using Vector=std::array<uint32_t,32>;
struct Access {
    uint32_t mask=UINT32_MAX, width=4, mode=0;
    bool write=false, atomic=false;
    Vector addresses{}, values{}, replacements{};
};
struct Driver {
    Bench<Vshared_memory> b{nullptr};
    uint64_t services=0, broadcasts=0;
    Driver() { b.dut.capacity=8; b.dut.response_latency=2; b.dut.atomic_latency=4; b.dut.eval(); }
    void edge() {
        auto& d=b.dut; services+=std::popcount(uint32_t{d.bank_accepted})+std::popcount(uint32_t{d.atomic_completed});
        for (uint32_t bank=0; bank<32; ++bank) if (d.bank_accepted & (1u<<bank)) broadcasts+=std::popcount(uint32_t{d.served_lanes[bank]})-1;
        b.tick(); d.request_valid=0; d.acknowledge=0; for (auto& mask : d.clear_rows) mask=0; d.eval();
    }
    uint32_t submit(const Access& a) {
        auto& d=b.dut; require(d.request_ready,"no input capacity"); const auto slot=d.request_slot;
        d.request_index=slot; d.request_mask=a.mask; d.request_width=a.width==1 ? 0 : a.width==2 ? 1 : 2;
        d.request_write=a.write; d.request_atomic=a.atomic; d.request_mode=a.mode;
        for (uint32_t lane=0; lane<32; ++lane) {
            if (lane%2==0) d.request_addresses[lane/2]=0;
            d.request_addresses[lane/2]|=a.addresses[lane]<<(lane%2*16);
            d.request_values[lane]=a.values[lane]; d.request_replacements[lane]=a.replacements[lane];
        }
        d.request_valid=1; d.eval(); require(!d.error,"valid request rejected"); edge(); return slot;
    }
    Vector take(uint32_t slot) {
        auto& d=b.dut; require(d.completed & (1u<<slot),"response not ready");
        d.query_slot=slot; d.eval(); Vector result{}; for (uint32_t lane=0; lane<32; ++lane) result[lane]=d.query_data[lane];
        d.acknowledge|=1u<<slot; d.eval(); require(!d.error,"valid response acknowledgement rejected"); return result;
    }
    Vector wait(uint32_t slot) {
        for (uint32_t count=0; count<10000; ++count) {
            if (b.dut.completed & (1u<<slot)) { auto result=take(slot); edge(); return result; }
            require(!b.dut.error,"pipeline protocol error"); edge();
        }
        throw std::runtime_error("shared response timeout");
    }
    Vector run(const Access& a) { return wait(submit(a)); }
};
Access sequence(uint32_t base=0,uint32_t stride=4,bool write=false) {
    Access a; a.write=write;
    for (uint32_t lane=0; lane<32; ++lane) { a.addresses[lane]=base+lane*stride; a.values[lane]=0x90123456u^(base+lane*7919); }
    return a;
}

void full_storage_and_allocation_clear() {
    Driver t;
    for (uint32_t row=0; row<512; ++row) t.run(sequence(row*128,4,true));
    for (uint32_t row=0; row<512; ++row) require(t.run(sequence(row*128))==sequence(row*128).values,"64 KiB data mismatch");
    t.b.dut.clear_rows[0]=1; t.b.dut.clear_rows[15]=uint32_t{1}<<31; t.b.dut.eval(); t.edge();
    require(t.run(sequence())==Vector{} && t.run(sequence(65408))==Vector{},"range clear leaked old bytes");
    require(t.run(sequence(128))==sequence(128).values,"clear touched neighboring region");
    auto partial=sequence(65409,0,true); partial.width=1; partial.mask=1; partial.values[0]=0xab; t.run(partial);
    auto value=t.run(sequence(65408,0)); for (auto word : value) require(word==0xab00,"first partial write retained stale bytes");
}
void banks_broadcast_subwords_and_conflicts() {
    Driver t; t.run(sequence(0,4,true)); auto a=sequence(0,0); a.width=1;
    for (uint32_t lane=0; lane<32; ++lane) a.addresses[lane]=lane%4;
    const auto services=t.services, broadcasts=t.broadcasts; const auto result=t.run(a);
    for (uint32_t lane=0; lane<32; ++lane) require(result[lane]==((0x90123456u>>(lane%4*8))&255),"subword broadcast data wrong");
    require(t.services-services==1 && t.broadcasts-broadcasts==31,"same-word read was not one bank service");
    a=sequence(0,0,true); t.run(a);
    require(t.run(sequence(0,0))[0]==a.values[31],"same-word writes not serialized by lane");
    auto start=t.b.cycle; t.run(sequence()); const auto spread=t.b.cycle-start;
    start=t.b.cycle; t.run(sequence(0,128)); require(t.b.cycle-start==spread+31,"32-way conflict did not add 31 services");
}

uint32_t reference_atomic(uint32_t mode,uint32_t old,uint32_t value,uint32_t replacement) {
    switch (mode) {
    case 0: return old+value; case 1: return value; case 2: return old==value ? replacement : old;
    case 3: return old & value; case 4: return old | value; case 5: return old ^ value;
    case 6: return std::min(old,value); case 7: return std::max(old,value);
    case 8: return std::bit_cast<uint32_t>(std::min(std::bit_cast<int32_t>(old),std::bit_cast<int32_t>(value)));
    case 9: return std::bit_cast<uint32_t>(std::max(std::bit_cast<int32_t>(old),std::bit_cast<int32_t>(value)));
    default: throw std::runtime_error("invalid reference mode");
    }
}
void atomics_and_oldest_request_order() {
    Driver t;
    for (uint32_t mode=0; mode<10; ++mode) {
        const auto initial=sequence(0,4,true); t.run(initial); auto a=sequence(); a.write=true; a.atomic=true; a.mode=mode;
        Vector expected{};
        for (uint32_t lane=0; lane<32; ++lane) {
            a.values[lane]=(lane%2==0) ? initial.values[lane] : lane*17527; a.replacements[lane]=lane+9;
            expected[lane]=reference_atomic(mode,initial.values[lane],a.values[lane],a.replacements[lane]);
        }
        require(t.run(a)==initial.values,"RMW did not return old value"); require(t.run(sequence())==expected,"atomic result mismatch");
    }
    auto zero=sequence(0,0,true); zero.mask=1; zero.values[0]=0; t.run(zero);
    auto add=zero; add.atomic=true; add.values[0]=1; t.b.dut.atomic_latency=9; t.b.dut.eval();
    std::array<uint32_t,8> slots{}; for (auto& slot : slots) slot=t.submit(add);
    require(!t.b.dut.request_ready,"unconsumed results did not retain capacity");
    require(t.wait(slots[0])[0]==0,"first atomic owner wrong"); const auto youngest=t.submit(add);
    for (uint32_t i=1; i<8; ++i) require(t.wait(slots[i])[0]==i,"reused physical slot bypassed older requests");
    require(t.wait(youngest)[0]==8,"new request did not remain youngest");
}

// A separate byte vector models subword overlap and all integer RMW operations.
// 独立字节向量建模子字重叠及全部整数读改写操作。
Vector update_reference(std::vector<uint8_t>& bytes,const Access& a) {
    Vector expected{};
    for (uint32_t lane=0; lane<32; ++lane) if (a.mask & (1u<<lane)) {
        uint32_t old=0; for (uint32_t i=0; i<a.width; ++i) old|=uint32_t{bytes[a.addresses[lane]+i]}<<(i*8);
        if (!a.write || a.atomic) expected[lane]=old;
        const auto next=a.atomic ? reference_atomic(a.mode,old,a.values[lane],a.replacements[lane]) : a.values[lane];
        if (a.write) for (uint32_t i=0; i<a.width; ++i) bytes[a.addresses[lane]+i]=static_cast<uint8_t>(next>>(i*8));
    }
    return expected;
}
void random_byte_differential() {
    Driver t; std::mt19937 rng(17591); std::vector<uint8_t> bytes(65536);
    for (uint32_t trial=0; trial<700; ++trial) {
        Access a; a.width=1u<<(rng()%3); a.mask=rng(); a.write=rng()%2;
        a.atomic=a.width==4 && a.write && rng()%2; a.mode=rng()%10;
        const auto common=(rng()%16384)*4;
        for (uint32_t lane=0; lane<32; ++lane) {
            a.addresses[lane]=trial%3==0 ? common : trial%3==1 ? (rng()%512)*128 : rng()%(65536/a.width)*a.width;
            a.values[lane]=rng(); a.replacements[lane]=rng();
        }
        require(t.run(a)==update_reference(bytes,a),"random byte/atomic result mismatch");
    }
    for (uint32_t row=0; row<512; ++row) { const auto a=sequence(row*128); require(t.run(a)==update_reference(bytes,a),"random final SRAM mismatch"); }
}
void invalid_events_and_reset() {
    Driver t; auto& d=t.b.dut; auto a=sequence(0,0,true); a.mask=1; a.atomic=true; a.values[0]=1;
    d.atomic_latency=UINT32_MAX; d.eval(); t.submit(a); t.edge();
    d.clear_rows[0]=1; d.eval(); require(d.error==5,"clear overlapped live atomic"); t.edge();
    require(!d.error && !d.completed,"rejected clear completed or destroyed atomic");
    d.acknowledge=1; d.eval(); require(d.error==2,"early acknowledgement accepted"); t.edge();
    for (uint32_t i=0; i<8; ++i) { require(!d.completed,"32-bit atomic delay truncated"); t.edge(); }
    d.reset=1; t.edge(); d.reset=0; d.atomic_latency=1; d.eval();
    require(!d.occupied && !d.completed,"reset retained requests"); require(t.run(sequence(0,0))==Vector{},"reset exposed old data");
    d.request_valid=1; d.request_width=3; d.eval(); require(d.error==3,"invalid width accepted"); t.edge();
    d.request_width=2; d.request_addresses[0]=1; d.request_mask=1; d.request_valid=1; d.eval();
    require(d.error==4,"misaligned write accepted"); t.edge(); require(t.run(sequence(0,0))==Vector{},"invalid batch partially wrote SRAM");
}
int run_application() {
    try {
        run_case("shared full 64 KiB SRAM, row clearing and partial-write zeroes",full_storage_and_allocation_clear);
        run_case("shared 32 banks, subword broadcast and serialized conflicts",banks_broadcast_subwords_and_conflicts);
        run_case("shared ten atomic modes, eight slots and age-preserving reuse",atomics_and_oldest_request_order);
        run_case("shared 700 randomized byte/atomic requests and full SRAM comparison",random_byte_differential);
        run_case("shared invalid events, full-width atomic delay and in-flight reset",invalid_events_and_reset); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

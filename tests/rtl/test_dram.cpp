#include "Vdram_controller.h"
#include "bench.hpp"
#include "simt/hardware_memory.hpp"
#include <deque>
#include <map>
#include <random>
#include <vector>

namespace {
using simt::hw::Dram; using simt::hw::DramConfig; using simt::hw::Packet; using simt::hw::Sector; using simt::hw::Ticket;

// External cells are modeled by a byte map, separate from the C++ controller's backing memory.
// 外部单元由字节映射表示，与 C++ 控制器的底层内存相互独立。
struct Driver {
    Bench<Vdram_controller> b{nullptr};
    simt::Memory cells;
    explicit Driver(const DramConfig& c={}) { auto& d=b.dut; d.rcd=c.rcd; d.cas=c.cas; d.rp=c.rp; d.burst=c.burst; d.eval(); }
    void pack(WData* words,const Sector& data) {
        for (uint32_t w=0; w<8; ++w) { uint32_t v=0; for (uint32_t i=0; i<4; ++i) v|=uint32_t{data[w*4+i]}<<(i*8); words[w]=v; }
    }
    Sector unpack(const WData* words) {
        Sector data{}; for (uint32_t w=0; w<8; ++w) for (uint32_t i=0; i<4; ++i) data[w*4+i]=static_cast<uint8_t>(words[w]>>(i*8));
        return data;
    }
    uint32_t request(const Packet& p) {
        auto& d=b.dut; d.eval(); require(d.request_ready,"request port unavailable");
        d.request_valid=1; d.request_write=p.write; d.request_address=p.address; d.request_mask=p.byte_mask;
        pack(d.request_data,p.data); d.eval(); return d.request_slot;
    }
    void tick() {
        auto& d=b.dut; require(!d.error,"unexpected DRAM error "+std::to_string(d.error)); d.eval();
        if (d.transfer_valid) {
            const auto data=unpack(d.transfer_data); Sector result{};
            for (uint32_t i=0; i<32; ++i) {
                const auto at=d.transfer_address+i;
                if (d.transfer_write && (d.transfer_mask&(1u<<i))) cells.write(at,1,data[i]);
                result[i]=static_cast<uint8_t>(cells.read(at,1));
            }
            pack(d.transfer_result,result); d.eval();
        }
        b.tick(); d.request_valid=0; d.acknowledge=0; d.eval();
    }
    bool ready(uint32_t slot) { b.dut.eval(); return b.dut.completed&(uint64_t{1}<<slot); }
    Sector take(uint32_t slot) {
        auto& d=b.dut; require(ready(slot),"response missing"); d.query_slot=slot; d.eval();
        const auto data=unpack(d.query_data); d.acknowledge|=uint64_t{1}<<slot; d.eval(); return data;
    }
};
Packet packet(uint64_t address,bool write=false,uint8_t fill=0) {
    Packet p; p.address=address; p.write=write; p.data.fill(fill); return p;
}
uint32_t latency(Driver& t,const Packet& p) {
    const auto slot=t.request(p); uint32_t clocks=0;
    do { t.tick(); ++clocks; require(clocks<1000,"DRAM timeout"); } while (!t.ready(slot));
    t.take(slot); t.tick(); return clocks;
}

// First, row-hit and row-conflict accesses use exactly the configured timing terms.
// 首次访问、行命中和行冲突都精确使用配置的时序项。
void row_timing_and_data() {
    Driver t; const DramConfig c;
    require(latency(t,packet(0,true,0x5a))==c.cas+c.rcd+c.burst,"closed-row latency wrong");
    require(latency(t,packet(32))==c.cas+c.burst,"row-hit latency wrong");
    const auto slot=t.request(packet(32)); while (!t.ready(slot)) t.tick();
    require(t.take(slot)==packet(0,true,0x5a).data,"read returned wrong data"); t.tick();
    require(latency(t,packet(uint64_t{2048}*8))==c.cas+c.rcd+c.rp+c.burst,"row-conflict latency wrong");
}

void byte_mask_and_invalid_inputs() {
    Driver t; auto p=packet(64,true,0xff); p.byte_mask=0x0000000f; latency(t,p);
    const auto slot=t.request(packet(64)); while (!t.ready(slot)) t.tick();
    const auto data=t.take(slot); require(data[3]==0xff && !data[4],"byte mask ignored"); t.tick();
    auto& d=t.b.dut; const auto now=d.now;
    d.request_valid=1; d.request_address=1; d.eval(); require(d.error==4,"unaligned address accepted"); t.b.tick(); d.request_valid=0; d.eval();
    d.acknowledge=1; d.eval(); require(d.error==2,"empty acknowledgement accepted"); t.b.tick(); d.acknowledge=0; d.eval();
    d.rcd=0; d.eval(); require(d.error==1,"zero timing accepted"); t.b.tick(); d.rcd=12; d.eval();
    require(d.now==now && !d.occupied,"invalid batch changed state");
}

void capacity_retained_until_taken() {
    Driver t; std::vector<uint32_t> slots;
    for (uint32_t n=0; n<64; ++n) { slots.push_back(t.request(packet(n*32))); t.tick(); }
    for (uint32_t n=0; n<2000; ++n) t.tick();
    auto& d=t.b.dut; d.eval(); require(d.completed==UINT64_MAX && !d.request_ready,"retained responses released capacity");
    t.take(slots[5]); d.eval(); require(d.request_ready && d.request_slot==slots[5],"acknowledged slot not reused");
    const auto again=t.request(packet(0,false)); t.tick(); while (!t.ready(again)) t.tick(); t.take(again); t.tick();
    for (uint32_t n=0; n<64; ++n) if (n!=5) t.take(slots[n]);
    t.tick(); require(!d.occupied,"slots leaked");
}

// The existing C++ controller is the cycle reference: every response must appear on the same clock.
// 现有 C++ 控制器作为逐拍参考：每个响应都必须在相同时钟出现。
struct Flight { uint32_t slot; Ticket ticket; };
void random_cycle_equivalence_with_model(uint32_t seed,uint32_t range) {
    Driver t; simt::Memory cells; Dram model(cells); std::mt19937 rng(seed); std::deque<Flight> flights; uint64_t responses=0;
    for (uint32_t clock=0; clock<30000; ++clock) {
        if (flights.size()<60 && rng()%3==0) {
            Packet p=packet(uint64_t{rng()%range}*32,rng()%3==0); p.byte_mask=rng();
            for (auto& byte : p.data) byte=static_cast<uint8_t>(rng());
            const auto ticket=model.submit(p); require(bool(ticket),"model rejected request");
            flights.push_back({t.request(p),*ticket});
        }
        t.tick(); model.tick();
        for (size_t at=0; at<flights.size();) {
            const auto expected=model.take(flights[at].ticket);
            require(expected.has_value()==t.ready(flights[at].slot),"response clock differs at "+std::to_string(clock));
            if (!expected) { ++at; continue; }
            require(t.take(flights[at].slot)==*expected,"response data differs");
            flights.erase(flights.begin()+static_cast<long>(at)); ++responses;
        }
        t.b.dut.eval();
    }
    require(responses>1000,"random traffic made too little progress");
}
int run_application() {
    try {
        run_case("DRAM row timing and external data",row_timing_and_data);
        run_case("DRAM byte mask and atomic invalid inputs",byte_mask_and_invalid_inputs);
        run_case("DRAM 64 slots retained until taken",capacity_retained_until_taken);
        run_case("DRAM 30000 clocks match C++ controller, row-local traffic",[]{ random_cycle_equivalence_with_model(9001,64); });
        run_case("DRAM 30000 clocks match C++ controller, conflicting traffic",[]{ random_cycle_equivalence_with_model(9002,1u<<16); }); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

#include "simt/hardware_memory.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt;
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool caught=false; try { action(); } catch (const std::exception& e) { caught=e.what()==expected; }
    require(caught,"wrong or missing shared adapter exception");
}
WarpAccess access(uint32_t address=0,uint32_t value=0,bool write=false) {
    WarpAccess a; a.mask=1; a.address[0]=address; a.value[0]=value; a.write=write; return a;
}
Ticket submit(SharedPort& unit,const WarpAccess& a,Memory& memory) {
    const auto ticket=unit.submit(a,&memory); require(bool(ticket),"shared submit rejected"); return *ticket;
}
std::array<uint32_t,32> wait(SharedPort& unit,Ticket ticket) {
    for (uint32_t count=0; count<10000; ++count) {
        if (auto result=unit.take(ticket)) return *result;
        unit.tick();
    }
    throw std::runtime_error("shared adapter timeout");
}
void storage_and_observation_are_independent() {
    auto unit=make_shared_engine({},true); Memory snapshot(65536); snapshot.write(0,4,99);
    unit->attach(snapshot,0,65536); require(!unit->cycle() && snapshot.read(0,4)==0,"allocation advanced clock or imported bytes");
    auto ticket=submit(*unit,access(65532,0x12345678,true),snapshot);
    require(!unit->cycle() && snapshot.read(65532,4)==0,"submit wrote bytes before an edge");
    rejects([&]{unit->detach(snapshot);},"shared_rtl_region_busy"); wait(*unit,ticket); unit->tick();
    require(snapshot.read(65532,4)==0x12345678,"actual write did not update observation"); snapshot.write(65532,4,99);
    ticket=submit(*unit,access(65532),snapshot);
    for (uint32_t query=0; query<20; ++query) require(!unit->take(ticket),"query produced hidden edges");
    require(wait(*unit,ticket)[0]==0x12345678,"load used software snapshot instead of SRAM"); unit->tick();
    require(unit->stats().accepted==2 && unit->stats().consumed==2,"accepted/consumed events wrong");
    unit->detach(snapshot); unit->attach(snapshot,0,65536);
    require(wait(*unit,submit(*unit,access(65532),snapshot))[0]==0,"reused region retained old physical bytes");
}

void regions_subwords_and_atomics() {
    auto unit=make_shared_engine({},true); Memory first(32768),second(32768);
    unit->attach(first,0,32768); unit->attach(second,32768,32768);
    wait(*unit,submit(*unit,access(0,17,true),first)); unit->tick();
    wait(*unit,submit(*unit,access(0,29,true),second)); unit->tick();
    auto byte=access(1,0xab,true); byte.width=1; wait(*unit,submit(*unit,byte,second)); unit->tick();
    require(wait(*unit,submit(*unit,access(),first))[0]==17,"CTA memory aliased"); unit->tick();
    auto atomic=access(0,1,true); atomic.atomic=0; atomic.mask=UINT32_MAX; atomic.value.fill(1);
    const auto returned=wait(*unit,submit(*unit,atomic,second)); unit->tick();
    for (uint32_t lane=0; lane<32; ++lane) require(returned[lane]==0xab1d+lane,"same-word atomic lost update or old value");
    require(second.read(0,4)==0xab3d && unit->stats().atomic_started==32 && unit->stats().atomic_completed==32,"atomic observation/events wrong");
    unit->detach(first); Memory replacement(32768); unit->attach(replacement,0,32768);
    require(wait(*unit,submit(*unit,access(),second))[0]==0xab3d,"allocation cleared another CTA");
}

// Consuming another response after driving a request must not change its selected physical slot.
// 驱动请求后消费另一响应，不得改变该请求已选择的物理槽。
void response_capacity_and_accepted_pin_stability() {
    auto unit=make_shared_engine({},true); Memory snapshot(65536); unit->attach(snapshot,0,65536);
    std::array<Ticket,8> tickets{};
    for (uint32_t i=0; i<8; ++i) { tickets[i]=submit(*unit,access(i*4,100+i,true),snapshot); unit->tick(); }
    for (uint32_t i=0; i<8; ++i) unit->tick();
    require(unit->outstanding()==8 && !unit->submit(access(),&snapshot),"unconsumed results released capacity");
    require(bool(unit->take(tickets[7])),"highest slot response missing");
    const auto new_ticket=submit(*unit,access(28),snapshot);
    require(bool(unit->take(tickets[0])),"old response not consumable after new offer"); unit->tick();
    for (uint32_t i=1; i<7; ++i) require(bool(unit->take(tickets[i])),"response changed during unrelated handshake");
    require(wait(*unit,new_ticket)[0]==107,"late acknowledgement redirected accepted request"); unit->tick();
    require(unit->stats().accepted==9 && unit->stats().consumed==9 && !unit->outstanding(),"transport accounting unbalanced");
    const auto active=unit->stats().active_clocks; unit->tick(); require(unit->stats().active_clocks==active,"idle storage clock not gated");
}
void validation_and_isolation() {
    auto unit=make_shared_engine({},true), other=make_shared_engine({},true); Memory a(256),b(256);
    unit->attach(a,0,256); other->attach(b,0,256);
    rejects([&]{unit->attach(b,128,256);},"overlapping_shared_rtl_region");
    rejects([&]{unit->submit(access(256),&a);},"memory_out_of_bounds");
    rejects([&]{unit->submit(access(1),&a);},"misaligned_memory_access");
    auto bad=access(0,1,true); bad.atomic=10;
    rejects([&]{unit->submit(bad,&a);},"invalid_atomic_access");
    wait(*unit,submit(*unit,access(0,73,true),a)); unit->tick();
    require(!other->cycle() && wait(*other,submit(*other,access(),b))[0]==0,"separate SMs shared storage");
    auto config=SharedConfig{}; config.banks=16;
    rejects([&]{make_shared_engine(config,true);},"invalid_shared_rtl_configuration");
    unit->detach(a); rejects([&]{unit->submit(access(),&a);},"unknown_shared_rtl_region");
}
void run_cases() {
    storage_and_observation_are_independent(); std::cout<<"PASS shared adapter real SRAM, explicit edges and reused region zeroes\n";
    regions_subwords_and_atomics(); std::cout<<"PASS shared adapter CTA regions, byte writes and actual atomic results\n";
    response_capacity_and_accepted_pin_stability(); std::cout<<"PASS shared adapter full response capacity and accepted pin stability\n";
    validation_and_isolation(); std::cout<<"PASS shared adapter validation, rejection and independent SM storage\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

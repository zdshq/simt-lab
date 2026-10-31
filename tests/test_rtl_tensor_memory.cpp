#include "simt/tensor_memory.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt;
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool caught=false; try { action(); } catch (const std::exception& e) { caught=e.what()==expected; }
    require(caught,"wrong or missing tensor memory exception");
}
WarpAccess access(uint32_t address=0,uint32_t value=0,bool write=false) {
    WarpAccess a; a.mask=1; a.address[0]=address; a.value[0]=value; a.write=write; return a;
}
Ticket submit(TensorMemory& unit,const WarpAccess& a,Memory& memory) {
    const auto ticket=unit.submit(a,&memory); require(bool(ticket),"TMEM submit rejected"); return *ticket;
}
std::array<uint32_t,32> wait(TensorMemory& unit,Ticket ticket) {
    for (uint32_t n=0; n<10000; ++n) {
        if (auto value=unit.take(ticket)) return *value;
        unit.tick();
    }
    throw std::runtime_error("TMEM adapter timeout");
}
void store(TensorMemory& unit,Memory& memory,uint32_t address,uint32_t value) {
    wait(unit,submit(unit,access(address,value,true),memory)); unit.tick();
}

// Modifying an observation must not affect a later read from the actual SRAM.
// 修改观测对象不能影响后续从实际 SRAM 读取的值。
void hardware_storage_and_explicit_edges() {
    auto unit=make_tensor_memory({},true); Memory memory(131072); unit->attach(memory,63,131072);
    require(unit->allocate(memory,256)==0 && !unit->columns() && !unit->cycle(),"allocation used hidden edge");
    rejects([&]{unit->coordinate(memory,127,255);},"tensor_column_not_allocated"); unit->tick();
    require(unit->columns()==256 && unit->coordinate(memory,127,255)==131068,"full geometry wrong");
    const auto ticket=submit(*unit,access(131068,0x12345678,true),memory);
    for (uint32_t n=0; n<20; ++n) require(!unit->take(ticket) && memory.read(131068,4)==0,"unclocked write became visible");
    wait(*unit,ticket); unit->tick(); require(memory.read(131068,4)==0x12345678,"bank write observation wrong");
    memory.write(131068,4,99);
    require(wait(*unit,submit(*unit,access(131068),memory))[0]==0x12345678,"read used software snapshot"); unit->tick();
    unit->detach(memory); require(unit->columns()==256,"retirement used hidden clock"); unit->tick();
    const auto active=unit->stats().active_clocks; unit->tick();
    require(!unit->columns() && !unit->pending() && unit->stats().active_clocks==active,"retirement or clock gate wrong");
}
void independent_owners_free_reuse_and_rejection() {
    auto unit=make_tensor_memory({},true); Memory first(131072),second(131072),replacement(131072);
    unit->attach(first,0,131072); unit->attach(second,63,131072);
    require(unit->allocate(first,8)==0,"first logical base wrong"); unit->tick();
    require(unit->allocate(second,8)==0,"second logical base wrong"); unit->tick();
    store(*unit,first,0,17); store(*unit,second,0,29);
    require(wait(*unit,submit(*unit,access(),first))[0]==17,"separate CTA memory aliased"); unit->tick();
    rejects([&]{unit->free(first,0,16);},"invalid_tensor_free"); require(unit->columns()==16,"failed free changed quota");
    unit->free(first,0,8); unit->tick(); rejects([&]{unit->coordinate(first,0,0);},"tensor_column_not_allocated");
    require(unit->allocate(first,8)==0,"freed logical base not reused"); unit->tick(); store(*unit,first,0,0);
    require(wait(*unit,submit(*unit,access(),second))[0]==29,"reuse cleared another CTA"); unit->tick();
    unit->detach(first); unit->attach(replacement,0,131072); unit->tick();
    unit->allocate(replacement,8); unit->tick(); store(*unit,replacement,0,41);
    require(wait(*unit,submit(*unit,access(),replacement))[0]==41,"CTA slot reuse failed"); unit->tick();
    unit->detach(second); unit->detach(replacement); unit->tick();
    const auto& s=unit->allocations(); require(s.allocated==s.freed+s.reclaimed && !unit->columns(),"column accounting leaked");
}
void backpressure_busy_owner_and_held_slot() {
    auto unit=make_tensor_memory({},true); Memory memory(131072); unit->attach(memory,0,131072); unit->allocate(memory,8); unit->tick();
    std::array<Ticket,8> tickets{};
    for (uint32_t slot=0; slot<8; ++slot) { tickets[slot]=submit(*unit,access(slot*4,slot+50,true),memory); unit->tick(); }
    for (uint32_t n=0; n<20; ++n) unit->tick();
    require(!unit->submit(access(),&memory) && unit->outstanding()==8,"response capacity not retained");
    rejects([&]{unit->free(memory,0,8);},"tensor_memory_owner_busy");
    rejects([&]{unit->detach(memory);},"tensor_memory_owner_busy");
    require(bool(unit->take(tickets[7])),"highest response missing"); const auto next=submit(*unit,access(28),memory);
    require(bool(unit->take(tickets[0])),"late acknowledgement rejected"); unit->tick();
    for (uint32_t slot=1; slot<7; ++slot) require(bool(unit->take(tickets[slot])),"response lost in handoff");
    require(wait(*unit,next)[0]==57,"held request moved after late acknowledgement"); unit->tick();
    unit->detach(memory); unit->tick(); require(unit->stats().accepted==unit->stats().consumed,"request events unbalanced");
}
void all_bank_geometries_and_isolation() {
    for (uint32_t banks : {1u,2u,4u,8u,16u,32u,64u,128u}) {
        TensorConfig config; config.tmem_banks=banks; config.tmem_columns=8;
        auto unit=make_tensor_memory(config,true), other=make_tensor_memory(config,true);
        Memory a(131072),b(131072); unit->attach(a,0,131072); other->attach(b,0,131072);
        unit->allocate(a,8); unit->tick(); store(*unit,a,4092,banks+9);
        require(wait(*unit,submit(*unit,access(4092),a))[0]==banks+9,"parameterized bank data wrong"); unit->tick();
        require(!other->cycle() && !other->columns(),"SM instances shared state");
        rejects([&]{unit->allocate(a,8);},"tensor_out_of_columns");
        rejects([&]{unit->coordinate(a,128,0);},"tensor_coordinate_out_of_range");
        rejects([&]{unit->submit(access(4096),&a);},"tensor_column_not_allocated");
        unit->detach(a); unit->tick();
    }
    TensorConfig invalid; invalid.tmem_banks=3;
    rejects([&]{make_tensor_memory(invalid,true);},"tensor_memory_rtl_requires_power_of_two_banks");
}
void run_cases() {
    hardware_storage_and_explicit_edges(); std::cout<<"PASS TMEM adapter real SRAM, observations and explicit edges\n";
    independent_owners_free_reuse_and_rejection(); std::cout<<"PASS TMEM adapter independent owners, exact free and CTA slot reuse\n";
    backpressure_busy_owner_and_held_slot(); std::cout<<"PASS TMEM adapter response capacity, busy ownership and held request\n";
    all_bank_geometries_and_isolation(); std::cout<<"PASS TMEM adapter eight bank geometries, quota and SM isolation\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

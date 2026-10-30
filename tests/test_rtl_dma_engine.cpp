#include "simt/dma_engine.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool failed=false; try { action(); } catch (const std::exception& e) { failed=e.what()==expected; }
    require(failed,"wrong or missing DMA exception");
}
DmaAccess wait_request(DmaEngine& unit,bool write) {
    for (uint32_t i=0; i<100; ++i) { if (auto r=unit.request(write)) return *r; unit.tick(); }
    throw std::runtime_error("DMA request timeout");
}
DmaCompletion wait_completion(DmaEngine& unit) {
    for (uint32_t i=0; i<100; ++i) { if (auto r=unit.completion()) return *r; unit.tick(); }
    throw std::runtime_error("DMA completion timeout");
}

// External memory supplies arbitrary payload bits; the adapter must preserve those bits and address carries.
// 外部内存提供任意载荷位；适配器必须保持这些位以及地址进位。
uint32_t transfer(DmaEngine& unit,const DmaDescriptor& c,uint32_t offset) {
    const auto read=wait_request(unit,false); const auto bytes=std::popcount(read.access.mask)*read.access.width;
    require(read.id==c.id && read.cta==c.cta && read.access.address[0]==uint64_t{c.source}+offset,"read owner/address differs");
    require(offset+bytes<=c.bytes,"read exceeds command"); unit.accept(false); unit.tick();
    std::array<uint32_t,32> data{};
    for (uint32_t lane=0; lane<32; ++lane) data[lane]=uint32_t(c.id)^0xdeadbeef^(offset*37+lane*19);
    const auto response=unit.returned_read(read.slot,data);
    require(response.id==c.id && response.bytes==bytes,"read return accounting differs"); unit.tick();
    const auto write=wait_request(unit,true);
    require(write.slot==read.slot && write.id==c.id && write.cta==c.cta && write.access.mask==read.access.mask &&
            write.access.width==read.access.width && write.access.value==data,"payload or fragment ownership differs");
    for (uint32_t lane=0; lane<32; ++lane) if (write.access.mask&(uint32_t{1}<<lane))
        require(write.access.address[lane]==uint64_t{c.destination}+offset+lane*write.access.width,"write address carry lost");
    unit.accept(true); unit.tick(); const auto ack=unit.returned_write(write.slot);
    require(ack.id==c.id && ack.bytes==bytes,"write return accounting differs"); unit.tick(); return bytes;
}
void transfer_all(DmaEngine& unit,const DmaDescriptor& c) {
    unit.submit(c); unit.tick();
    for (uint32_t offset=0; offset<c.bytes;) offset+=transfer(unit,c,offset);
}
void release(DmaEngine& unit,const DmaCompletion& c) {
    unit.claim(); unit.tick(); unit.release(c); unit.tick();
}
void clocks_held_offers_and_credit() {
    auto unit=make_dma_engine(1,1,128); DmaDescriptor c{0x12345678abcdef01ull,99,4096,128,1024,17,128};
    require(!unit->stats().clocks && !unit->commands(),"constructor counted hidden clocks"); unit->submit(c);
    require(!unit->commands() && !unit->stats().submitted,"submit clocked descriptor");
    rejects([&]{unit->submit(c);},"dma_command_phase"); unit->tick(); const auto r=wait_request(*unit,false);
    const auto clock=unit->stats().clocks;
    for (uint32_t i=0; i<20; ++i) {
        auto snapshot=*unit->request(false); snapshot.access.address[0]=3;
        require(unit->request(false)->access.address[0]!=snapshot.access.address[0],"observation overwrote hardware");
    }
    require(unit->stats().clocks==clock && unit->request(false)->slot==r.slot,"observation advanced clocks");
    transfer(*unit,c,0); const auto complete=wait_completion(*unit);
    require(complete.id==c.id && complete.cta==99 && complete.barrier==1024 && complete.phase==17 && complete.bytes==128,"completion fields lost");
    unit->claim(); require(!unit->completion(),"claim consumed twice"); unit->tick();
    for (uint32_t i=0; i<15; ++i) { unit->tick(); require(unit->commands()==1 && !unit->available(),"claim freed slot before publication"); }
    auto wrong=complete; ++wrong.id; rejects([&]{unit->release(wrong);},"dma_invalid_release");
    unit->release(complete); require(unit->commands()==1,"release had hidden clock"); unit->tick();
    const auto active=unit->stats().active_clocks; unit->tick(); unit->tick();
    require(!unit->commands() && !unit->fragments() && unit->stats().active_clocks==active,"idle clock gating or credit leak");
}
void carry_tail_and_handshake_statistics() {
    auto unit=make_dma_engine(1,1,128);
    for (uint32_t bytes : {1u,3u,127u,128u,129u,4096u}) {
        DmaDescriptor c{bytes,31,0xfffffff0u,0xfffffff8u,0xffffffe0u,0xfffffffeu,bytes};
        transfer_all(*unit,c); release(*unit,wait_completion(*unit));
    }
    const auto& s=unit->stats();
    require(s.submitted==6 && s.released==6 && s.completion_claims==6,"command event accounting");
    require(s.read_bytes==4484 && s.write_bytes==4484,"byte event accounting");
    require(s.fragments==s.reads && s.reads==s.writes && s.reads==s.read_returns && s.reads==s.write_returns,"fragment event accounting");
    require(s.peak_commands==1 && s.peak_fragments==1 && s.active_clocks<=s.clocks,"occupancy or clock accounting");
}
void independent_instances_and_physical_capacity() {
    auto a=make_dma_engine(64,64,128), b=make_dma_engine(1,1,1);
    for (uint32_t i=0; i<64; ++i) { a->submit({i,i,0,128,0,0,128}); a->tick(); }
    for (uint32_t i=0; i<5; ++i) a->tick();
    require(a->commands()==64 && a->fragments()==64 && !a->available(),"full physical capacities not available");
    require(b->available() && !b->commands() && !b->fragments() && !b->stats().clocks,"instances share state");
    DmaDescriptor c{0,3,1,129,0,0,3}; transfer_all(*b,c); release(*b,wait_completion(*b));
    require(b->stats().fragments==3 && a->commands()==64,"instance quota or independent data path failed");
}
void rejected_inputs_leave_ports_usable() {
    rejects([]{make_dma_engine(0,1,1);},"invalid_dma_configuration");
    rejects([]{make_dma_engine(65,1,1);},"invalid_dma_configuration");
    rejects([]{make_dma_engine(1,65,1);},"invalid_dma_configuration");
    rejects([]{make_dma_engine(1,1,129);},"invalid_dma_configuration");
    auto unit=make_dma_engine(2,1,128); DmaDescriptor c{99,0,0,128,0,0,4};
    for (uint32_t bytes : {0u,4097u}) { auto bad=c; bad.bytes=bytes; rejects([&]{unit->submit(bad);},"invalid_async_copy_size"); }
    rejects([&]{unit->returned_read(63,{});},"dma_invalid_read_return");
    rejects([&]{unit->returned_write(63);},"dma_invalid_write_return");
    rejects([&]{unit->accept(false);},"dma_request_phase");
    rejects([&]{unit->claim();},"dma_completion_phase");
    unit->submit(c); unit->tick(); rejects([&]{unit->submit(c);},"duplicate_dma_id");
    transfer(*unit,c,0); release(*unit,wait_completion(*unit));
    require(unit->stats().submitted==1 && unit->stats().released==1,"rejected event was counted");
}
void run_cases() {
    clocks_held_offers_and_credit(); std::cout<<"PASS DMA adapter explicit clocks, snapshots and publication credit\n";
    carry_tail_and_handshake_statistics(); std::cout<<"PASS DMA adapter raw payloads, 33-bit addresses, tails and event counts\n";
    independent_instances_and_physical_capacity(); std::cout<<"PASS DMA adapter full 64-slot geometry and independent instances\n";
    rejected_inputs_leave_ports_usable(); std::cout<<"PASS DMA adapter rejected inputs and recovery without ghost events\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

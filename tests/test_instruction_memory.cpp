#include "hardware_support.hpp"
#include "simt/hardware_context.hpp"

namespace simt::hw::test {
std::vector<InstructionWords> code(uint32_t count,uint32_t bias=0) {
    std::vector<InstructionWords> result(count);
    for (uint32_t pc=0; pc<count; ++pc) result[pc]={0x12345678u+bias+pc,pc+1,pc+2,pc+3};
    return result;
}
WarpAccess instruction_access(uint64_t address) {
    auto access=sequence(address); access.mask=15; return access;
}

void physical_decode_and_immutable_padding() {
    Memory global; HardwareContext h(global,{}); auto words=code(3);
    global.write(0,4,91); InstructionMapping m(h.instructions,words);
    require(m.address()==InstructionMemory::base && m.words()==3 && h.instructions.mappings()==1,"code mapping geometry");
    words[0][0]=0;
    require(h.instructions.read(m.address(),4)==0x12345678 && h.instructions.read(m.address()+1,1)==0x56,"immutable little-endian image");
    require(h.instructions.read(m.address()+2,2)==0x1234 && h.instructions.read(m.address()+60,4)==0,"halfword or last line padding");
    require(h.instructions.read(0,4)==91,"global address space changed");
    std::vector<Memory> threads(1); threads[0].write(0,4,37); h.physical.attach(0,threads);
    require(h.instructions.read(LocalAddressSpace::base,4)==37,"local address space changed");
    rejects([&]{h.instructions.write(m.address(),4,19);},"write_to_instruction_memory");
    rejects([&]{h.instructions.read(m.address()+1,4);},"misaligned_memory_access");
    rejects([&]{h.instructions.check(m.address()+32,33);},"instruction_mapping_range");
    rejects([&]{h.instructions.read(m.address()+64,4);},"unmapped_instruction_physical_address");
    rejects([&]{h.instructions.attach({});},"invalid_instruction_image_size");
    auto write=instruction_access(m.address()); write.write=true;
    rejects([&]{h.memory.submit(0,write);},"write_to_instruction_memory");
    require(h.memory.drain_owner(0) && h.memory.l2_stats().writes==0,"forbidden write reserved work");
}

void unique_addresses_and_mapping_lifetime() {
    Memory global; HardwareContext h(global,{}); uint64_t old=0;
    {
        InstructionMapping a(h.instructions,code(1)), b(h.instructions,code(5,11)); old=a.address();
        require(b.address()==old+64 && h.instructions.read(b.address(),4)==0x12345683,"mapping overlap");
        rejects([&]{h.instructions.check(a.address()+48,32);},"instruction_mapping_range");
    }
    require(h.instructions.mappings()==0,"destroyed mappings leaked backing bytes");
    rejects([&]{h.instructions.read(old,4);},"unmapped_instruction_physical_address");
    InstructionMapping next(h.instructions,code(1,29));
    require(next.address()==old+192 && h.instructions.read(next.address(),4)==0x12345695,"new mapping reused stale physical tag");
}

void physical_namespace_boundary() {
    Memory global; HardwareContext h(global,{});
    const auto count=(LocalAddressSpace::limit-LocalAddressSpace::base)/LocalAddressSpace::warp_bytes;
    require(LocalAddressSpace::translate(count-1,31,16383)==InstructionMemory::base-1,"last local byte");
    rejects([&]{LocalAddressSpace::translate(count,0,0);},"invalid_local_translation");
    std::vector<Memory> threads(1);
    rejects([&]{h.physical.attach(count,threads);},"invalid_local_warp_registration");
    require(h.reserve_local(count)==0,"full local namespace reservation");
    rejects([&]{h.reserve_local(1);},"local_physical_ids_exhausted");
    rejects([&]{h.instructions.check(InstructionMemory::base-1,2);},"crossing_instruction_address_space");
}

// Instruction and data reads share the same L2 sectors, DRAM controller and finite response credits.
// 指令与数据读取共享同一 L2 sector 通路、DRAM 控制器及有限响应额度。
void shared_hierarchy_and_stale_tag_isolation() {
    Memory global; HardwareContext h(global,{}); const auto tick=[&]{h.tick();}; uint64_t old=0;
    {
        InstructionMapping a(h.instructions,code(4)); old=a.address();
        auto first=await(h.memory,accepted(h.memory.submit(0,instruction_access(old),1,true)),tick);
        require(first[0]==0x12345678 && h.memory.dram_stats().reads==1,"instruction data did not cross DRAM");
        require(await(h.memory,accepted(h.memory.submit(1,instruction_access(old+16),1,true)),tick)[0]==0x12345679,"L2 sector half selection");
        require(h.memory.dram_stats().reads==1 && h.memory.l2_stats().hits==1,"second instruction did not share L2 bytes");
        require(h.memory.l1_stats(0).reads==0 && h.memory.l1_stats(1).reads==0,"code polluted data L1");
    }
    InstructionMapping b(h.instructions,code(4,99));
    require(b.address()!=old && await(h.memory,accepted(h.memory.submit(0,instruction_access(b.address()),2,true)),tick)[0]==0x123456db,
        "new mapping consumed old cached code");
    require(h.memory.dram_stats().reads==2,"new physical code incorrectly hit old L2 tag");
}

void instruction_drain_preserves_foreign_response() {
    Memory global; HierarchyConfig config; config.capacity=2; HardwareContext h(global,config);
    InstructionMapping m(h.instructions,code(4)); const auto owner=h.new_owner(), other=h.new_owner();
    accepted(h.memory.submit(0,instruction_access(m.address()),owner,true));
    auto data=sequence(0); data.mask=1; global.write(0,4,77);
    const auto ticket=accepted(h.memory.submit(1,data,other));
    require(!h.memory.submit(0,data),"response capacity ignored");
    require(!h.memory.drain_owner(owner),"pending instruction erased early");
    for (uint32_t cycle=0; cycle<1000 && !h.memory.drain_owner(owner); ++cycle) h.tick();
    require(h.memory.drain_owner(owner),"instruction owner did not drain");
    require(await(h.memory,ticket,[&]{h.tick();})[0]==77,"fault drain consumed another engine's response");
}

void run_cases() {
    run("instruction_physical_decode_and_immutable_padding",physical_decode_and_immutable_padding);
    run("instruction_unique_addresses_and_mapping_lifetime",unique_addresses_and_mapping_lifetime);
    run("instruction_physical_namespace_boundary",physical_namespace_boundary);
    run("instruction_shared_hierarchy_and_stale_tag_isolation",shared_hierarchy_and_stale_tag_isolation);
    run("instruction_drain_preserves_foreign_response",instruction_drain_preserves_foreign_response);
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return simt::hw::test::run_application(); }

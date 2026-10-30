#include "simt/machine.hpp"
#include "simt/hardware_context.hpp"
#include "simt/cache_engine.hpp"
#include "hardware_support.hpp"

namespace simt::hw::test {
Module program(uint32_t value=0x23456789) {
    const std::array<InstructionWords,3> words{{{1,0x700,value,0},{0x003f0023,0x00400700,0,0},{24,0x700,0,0}}};
    std::vector<uint8_t> bytes;
    for (const auto& instruction : words) for (auto word : instruction)
        for (uint32_t byte=0; byte<4; ++byte) bytes.push_back(uint8_t(word>>(byte*8)));
    return Module::from_bytes(bytes);
}
Config configuration(bool cache) {
    Config c; c.timing=true; c.sms=2; c.threads=1; c.blocks=2; c.cache_rtl=cache;
    c.pipeline.register_rtl=true; c.pipeline.collector_rtl=true; return c;
}

// Fail while both SMs have accepted instruction reads, then advance only the shared memory clock.
// 在两个 SM 都已接受取指请求时失败，然后只推进共享存储时钟。
void failure_retains_code_until_requests_drain(bool cache) {
    auto global=std::make_shared<Memory>(); HierarchyConfig hc; hc.cache_rtl=cache;
    auto hardware=std::make_shared<HardwareContext>(*global,hc); auto c=configuration(cache); c.max_cycles=8;
    {
        Machine m(program(),c,global,hardware); bool failed=false;
        for (uint32_t cycle=0; cycle<20 && !failed; ++cycle) {
            hardware->tick();
            try { require(!m.advance(),"short deadline unexpectedly completed"); }
            catch (const std::exception& e) { require(std::string(e.what())=="cycle_limit_exceeded","wrong launch failure"); failed=true; }
        }
        require(failed && hardware->instructions.mappings()==1,"failed launch released code early");
        require(!m.drain(),"accepted instruction requests were not retained");
        bool drained=false;
        for (uint32_t cycle=0; cycle<20000 && !drained; ++cycle) { hardware->tick(); drained=m.drain(); }
        require(drained && hardware->memory.dram_stats().instruction_reads==1,"drain lost or duplicated shared-sector reads");
    }
    require(hardware->instructions.mappings()==0,"failed launch leaked code mapping");
    c.max_cycles=100000;
    for (auto value : {17u,91u,17u}) {
        Machine m(program(value),c,global,hardware); bool done=false;
        for (uint32_t cycle=0; cycle<100000 && !done; ++cycle) { hardware->tick(); done=m.advance(); }
        require(done && global->read(0,4)==value,"next launch executed old code or stalled after drain");
    }
    require(hardware->instructions.mappings()==0,"completed launches leaked code mappings");
}

void run_cases() {
    failure_retains_code_until_requests_drain(false); std::cout<<"PASS actual kernel instruction failure drain and new images with model L2\n";
    if (cache_rtl_available()) {
        failure_retains_code_until_requests_drain(true); std::cout<<"PASS actual kernel instruction failure drain and new images with RTL L2\n";
    }
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return simt::hw::test::run_application(); }

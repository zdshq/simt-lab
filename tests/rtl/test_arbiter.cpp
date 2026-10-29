#include "bench.hpp"
#include "Vregister_arbiter.h"
#include "simt/hardware_pipeline.hpp"
#include <array>
#include <random>

namespace {
uint32_t registers(const std::array<uint32_t, 4>& values) {
    uint32_t packed = 0;
    for (uint32_t request = 0; request < 4; ++request) packed |= values[request] << (request*6);
    return packed;
}

void conflict_fairness_and_zero_register() {
    Bench<Vregister_arbiter> b("arbiter-fairness.vcd");
    b.dut.read_valid = b.dut.write_valid = 15;
    b.dut.read_register = registers({0, 4, 8, 12}); b.dut.write_register = registers({1, 5, 9, 13});
    for (uint32_t cycle = 0; cycle < 16; ++cycle) {
        b.dut.eval();
        require(b.dut.read_grant == (1u << (cycle%4)) && b.dut.write_grant == (1u << (cycle%4)), "unfair bank arbitration");
        b.tick();
    }
    b.dut.read_register = b.dut.write_register = registers({63, 63, 63, 63}); b.dut.eval();
    require(b.dut.read_grant == 15 && b.dut.write_grant == 15, "zero register consumed bank");
}

// Reorder C++ primitive calls by each bank's retained cursor, then compare exact hardware grants.
// 按各 bank 保留的游标重排 C++ 原语调用，再比较精确硬件授权。
uint32_t reference_grants(simt::hw::RegisterPorts& ports, std::array<uint32_t, 4>& cursors,
                          const std::array<uint32_t, 4>& regs, uint32_t valid, bool write, uint64_t cycle) {
    uint32_t grants = 0;
    for (uint32_t bank = 0; bank < 4; ++bank) {
        const auto start = cursors[bank];
        for (uint32_t offset = 0; offset < 4; ++offset) {
            const auto request = (start+offset)%4, reg = regs[request];
            if (!(valid & (1u << request)) || reg == 63 || reg%4 != bank) continue;
            if (write ? ports.write(reg, cycle) : ports.read(reg, cycle)) {
                grants |= 1u << request; cursors[bank] = (request+1)%4;
            }
        }
    }
    for (uint32_t request = 0; request < 4; ++request)
        if (regs[request] == 63 && (valid & (1u << request))) grants |= 1u << request;
    return grants;
}

void randomized_cpp_port_agreement() {
    Bench<Vregister_arbiter> b("arbiter-differential.vcd"); simt::hw::RegisterPorts ports(4);
    std::array<uint32_t, 4> read_cursor{}, write_cursor{}, reads{}, writes{};
    std::mt19937 rng(971);
    for (uint32_t cycle = 0; cycle < 4000; ++cycle) {
        for (uint32_t request = 0; request < 4; ++request) { reads[request] = rng()%64; writes[request] = rng()%64; }
        b.dut.read_valid = rng()%16; b.dut.write_valid = rng()%16;
        b.dut.read_register = registers(reads); b.dut.write_register = registers(writes); b.dut.eval();
        require(b.dut.read_grant == reference_grants(ports, read_cursor, reads, b.dut.read_valid, false, cycle), "read grant differs");
        require(b.dut.write_grant == reference_grants(ports, write_cursor, writes, b.dut.write_valid, true, cycle), "write grant differs");
        b.tick();
    }
}

void run_cases() {
    run_case("RTL bank arbiter conflicts/fairness/zero", conflict_fairness_and_zero_register);
    run_case("RTL bank arbiter 4000-clock C++ port agreement", randomized_cpp_port_agreement);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

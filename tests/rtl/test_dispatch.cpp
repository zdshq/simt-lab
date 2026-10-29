#include "bench.hpp"
#include "Vcollector_dispatch.h"
#include <algorithm>
#include <array>
#include <bit>
#include <numeric>
#include <random>
#include <type_traits>

namespace {
constexpr uint32_t count = COLLECTOR_ENTRIES, index_bits = std::countr_zero(count);
using Ranks = std::array<uint32_t,count>;
using Pipes = std::array<uint32_t,count>;

// Pack fields independently of the circuit implementation, including fields crossing 32-bit words.
// 独立于电路实现打包字段，包括跨越 32 位字边界的字段。
template<class Bits> void put(Bits& bits, uint32_t offset, uint32_t width, uint32_t value) {
    for (uint32_t index = 0; index < width; ++index) {
        const auto at = offset+index;
        if constexpr (std::is_integral_v<Bits>) {
            const auto mask = uint64_t{1}<<at;
            bits = (bits & ~mask) | (uint64_t{(value>>index)&1}<<at);
        } else {
            const auto mask = uint32_t{1}<<(at%32);
            bits[at/32] = (bits[at/32] & ~mask) | (((value>>index)&1)<<(at%32));
        }
    }
}

void configure(Vcollector_dispatch& dut, const Ranks& ranks, const Pipes& pipes) {
    for (uint32_t index = 0; index < count; ++index) {
        put(dut.ranks,index*6,6,ranks[index]); put(dut.pipes,index*3,3,pipes[index]);
    }
    dut.eval();
}

void compare(const Vcollector_dispatch& dut, const Ranks& ranks, const Pipes& pipes) {
    uint32_t valid = 0;
    for (uint32_t port = 0; port < 7; ++port) {
        uint32_t expected = count, age = count;
        for (uint32_t index = 0; index < count; ++index)
            if (dut.enable && (dut.ready_entries & dut.eligible_entries & (1u<<index)) &&
                pipes[index] == port && ranks[index] < age) { expected = index; age = ranks[index]; }
        if (expected == count) continue;
        valid |= 1u<<port;
        require(((dut.entries>>(port*index_bits)) & (count-1)) == expected, "dispatch selected wrong age/pipe");
    }
    require(dut.valid == valid, "dispatch valid mask differs");
}

void independent_ports_and_enable() {
    Vcollector_dispatch dut; Ranks ranks; Pipes pipes;
    for (uint32_t i = 0; i < count; ++i) { ranks[i] = i; pipes[i] = i%7; }
    dut.enable = 1; dut.ready_entries = UINT32_MAX; dut.eligible_entries = UINT32_MAX;
    configure(dut,ranks,pipes); compare(dut,ranks,pipes);
    dut.enable = 0; dut.eval(); require(!dut.valid && !dut.entries, "disabled dispatcher issued work");
    dut.final();
}

void age_and_entry_eligibility() {
    Vcollector_dispatch dut; Ranks ranks; Pipes pipes{};
    for (uint32_t i = 0; i < count; ++i) ranks[i] = count-i-1;
    dut.enable = 1; dut.ready_entries = UINT32_MAX; dut.eligible_entries = UINT32_MAX;
    configure(dut,ranks,pipes); compare(dut,ranks,pipes);
    require((dut.entries & (count-1)) == count-1, "low physical index replaced oldest rank");
    dut.eligible_entries &= ~(1u<<(count-1)); dut.eval(); compare(dut,ranks,pipes);
    dut.ready_entries &= ~(1u<<(count-2)); dut.eval(); compare(dut,ranks,pipes);
    dut.eligible_entries = 0; dut.eval(); require(!dut.valid, "ineligible request issued"); dut.final();
}

void randomized_permutations() {
    Vcollector_dispatch dut; Ranks ranks; Pipes pipes; std::mt19937 random(20261103);
    std::iota(ranks.begin(),ranks.end(),0);
    for (uint32_t round = 0; round < 5000; ++round) {
        std::shuffle(ranks.begin(),ranks.end(),random);
        for (auto& pipe : pipes) pipe = random()%8;
        dut.enable = random()%4 != 0; dut.ready_entries = random(); dut.eligible_entries = random();
        configure(dut,ranks,pipes); compare(dut,ranks,pipes);
    }
    dut.final();
}

void run_cases() {
    run_case("dispatch seven independent ports and enable",independent_ports_and_enable);
    run_case("dispatch age order and per-entry eligibility",age_and_entry_eligibility);
    run_case("dispatch 5000 independent randomized permutations",randomized_permutations);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

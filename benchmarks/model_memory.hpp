#pragma once
#include "simt/hardware_memory.hpp"
#include "memory_pattern.hpp"
#include <iostream>
#include <stdexcept>

namespace calibration::model {
using namespace simt::hw;
inline uint32_t number(const char* text, uint32_t low, uint32_t high) {
    size_t end = 0; const auto value = std::stoull(text,&end);
    if (text[0] == '-' || text[end] || value < low || value > high)
        throw std::runtime_error("invalid argument");
    return static_cast<uint32_t>(value);
}
inline WarpAccess addresses(uint32_t stride, uint32_t offset) {
    WarpAccess access;
    for (uint32_t lane = 0; lane < 32; ++lane) access.address[lane] = (lane*stride+offset)*4;
    return access;
}
template<class Port> auto receive(Port& port, std::optional<Ticket> ticket) {
    if (!ticket) throw std::runtime_error("unexpected rejected request");
    for (uint32_t clock = 0; clock < 4096; ++clock) {
        if (auto result = port.take(*ticket)) return *result;
        port.tick();
    }
    throw std::runtime_error("model request timeout");
}
inline void coalescing(uint32_t stride, uint32_t offset) {
    const auto packets = coalesce(addresses(stride,offset));
    std::cout << "{\"experiment\":\"coalesce\",\"stride_words\":" << stride
              << ",\"offset_words\":" << offset << ",\"sectors\":" << packets.size() << ",\"addresses\":[";
    for (size_t i = 0; i < packets.size(); ++i) std::cout << (i ? "," : "") << packets[i].packet.address;
    std::cout << "]}\n";
}
inline void shared(uint32_t stride, uint32_t iterations) {
    BankedShared memory;
    for (uint32_t i = 0; i < 4096; ++i) memory.memory.write(i*4,4,i^2048);
    auto access = addresses(stride,0);
    for (uint32_t i = 0; i < iterations; ++i) {
        const auto values = receive(memory,memory.submit(access));
        for (uint32_t lane = 0; lane < 32; ++lane) {
            if (values[lane] != ((lane*stride)^(((i+1)&1)*2048))) throw std::runtime_error("shared mismatch");
            access.address[lane] = values[lane]*4;
        }
    }
    const auto& stats = memory.stats();
    std::cout << "{\"experiment\":\"shared\",\"stride_words\":" << stride << ",\"iterations\":" << iterations
              << ",\"model_cycles\":" << memory.cycle() << ",\"bank_services\":" << stats.bank_services
              << ",\"broadcast_lanes\":" << stats.broadcasts << ",\"check_passed\":true}\n";
}

inline uint32_t chain(MemoryHierarchy& memory, uint32_t at, uint32_t steps, bool bypass) {
    WarpAccess access; access.mask = 1;
    for (uint32_t i = 0; i < steps; ++i) {
        access.address[0] = uint64_t{at}*4;
        at = receive(memory,memory.submit(0,access,0,bypass))[0];
    }
    return at;
}
inline void chase(bool bypass, uint32_t bytes, uint32_t steps, uint32_t seed) {
    if (bytes%32) throw std::runtime_error("working set must contain whole sectors");
    const auto values = ring(bytes,seed); simt::Memory backing;
    for (size_t i = 0; i < values.size(); i += 8) backing.write(i*4,4,values[i]);
    MemoryHierarchy memory(backing);
    if (chain(memory,0,bytes/32,bypass) != 0) throw std::runtime_error("broken cycle");
    const auto start = memory.cycle(); const auto l1 = memory.l1_stats(0), l2 = memory.l2_stats();
    const auto result = chain(memory,0,steps,bypass);
    if (result != follow(values,steps)) throw std::runtime_error("chase mismatch");
    std::cout << "{\"experiment\":\"chase\",\"policy\":\"" << (bypass ? "cg" : "ca") << "\",\"bytes\":" << bytes
              << ",\"iterations\":" << steps << ",\"seed\":" << seed << ",\"model_cycles\":" << memory.cycle()-start
              << ",\"l1_reads\":" << memory.l1_stats(0).reads-l1.reads << ",\"l1_hits\":" << memory.l1_stats(0).hits-l1.hits
              << ",\"l2_reads\":" << memory.l2_stats().reads-l2.reads << ",\"l2_hits\":" << memory.l2_stats().hits-l2.hits
              << ",\"l1_bytes\":" << memory.l1_cache(0).config().bytes << ",\"l2_bytes\":" << memory.l2_cache().config().bytes
              << ",\"final_value\":" << result << ",\"check_passed\":true}\n";
}
}

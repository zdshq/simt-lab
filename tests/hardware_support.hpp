#pragma once
#include "simt/hardware_memory.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace simt::hw::test {
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void rejects(F action, const std::string& message) {
    try { action(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(message) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("missing expected rejection: " + message);
}

// Watchdogs catch deadlocks without using wall-clock sleep or implementation-specific state.
// 看门狗发现死锁，不依赖实际睡眠时间或实现内部状态。
template<class T, class F> auto await(T& unit, Ticket ticket, F tick) {
    for (uint32_t cycle = 0; cycle < 10000; ++cycle) {
        auto value = unit.take(ticket);
        if (value) return *value;
        tick();
    }
    throw std::runtime_error("hardware_test_timeout");
}

inline Ticket accepted(std::optional<Ticket> ticket) {
    require(ticket.has_value(), "unexpected request backpressure");
    return *ticket;
}

inline WarpAccess sequence(uint64_t base, uint32_t stride = 4) {
    WarpAccess access;
    for (uint32_t lane = 0; lane < 32; ++lane) access.address[lane] = base + lane * stride;
    return access;
}

struct CacheRig {
    Memory memory;
    Dram dram;
    SectorCache cache;
    explicit CacheRig(CacheConfig config = {256, 128, 2, 4, 2, 2, 4, 16})
        : dram(memory, {2, 128, 16, 3, 2, 4, 1}), cache(dram, config) {}
    void tick() { dram.tick(); cache.tick(); }
    Sector read(uint64_t address) {
        return await(cache, accepted(cache.submit(Packet{address})), [this] { tick(); });
    }
};

inline void run(const char* name, const std::function<void()>& test) {
    test();
    std::cout << "PASS " << name << '\n';
}
}

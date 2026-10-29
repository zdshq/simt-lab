#include "hardware_support.hpp"
#include "simt/cache_engine.hpp"
#include "simt/hardware_context.hpp"
#include <random>

namespace simt::hw::test {
namespace {
struct Rig {
    Memory backing;
    Dram dram{backing};
    std::unique_ptr<CachePort> cache = make_cache(dram, {}, true, CacheLevel::l1);
    void tick() { dram.tick(); cache->tick(); }
    Ticket submit(const Packet& packet) {
        for (uint32_t clock = 0; clock < 10000; ++clock) {
            if (auto ticket = cache->submit(packet)) return *ticket;
            tick();
        }
        throw std::runtime_error("RTL cache submission timeout");
    }
    Sector finish(Ticket ticket) { return await(*cache, ticket, [this] { tick(); }); }
    Sector read(uint64_t address) { return finish(submit({address})); }
};

void explicit_clock_and_sram() {
    Rig rig; rig.backing.write(0, 4, 37);
    require(rig.cache->name() == "rtl", "silent model fallback");
    for (uint32_t attempt = 0; attempt < 20; ++attempt)
        require(!rig.cache->submit({0}), "unclocked reset accepted request");
    require(rig.cache->cycle() == 0 && rig.dram.cycle() == 0, "API advanced a hidden clock");
    require(rig.read(0)[0] == 37, "cold fill data"); rig.backing.write(0, 4, 99);
    const auto before = rig.cache->cycle(); const auto ticket = rig.submit({0});
    require(!rig.cache->submit({32}), "more than one input reserved per clock");
    require(!rig.cache->take(ticket) && rig.cache->cycle() == before, "query clocked the array");
    require(rig.finish(ticket)[0] == 37, "hit bypassed SRAM");
    require(rig.cache->cycle()-before == 5, "registered response handshake latency");
    require(rig.dram.stats().reads == 1 && rig.cache->stats().hits == 1, "RTL events not observed");
}

void bounded_responses_and_id_reuse() {
    Rig rig; std::array<Ticket,8> tickets{};
    for (uint32_t id = 0; id < tickets.size(); ++id) {
        rig.backing.write(id*32, 4, id+11); tickets[id] = rig.submit({id*32}); rig.tick();
    }
    for (uint32_t clock = 0; clock < 200; ++clock) rig.tick();
    require(rig.cache->outstanding() == 8 && !rig.cache->submit({4096}), "unconsumed responses freed capacity");
    for (uint32_t id = 0; id < tickets.size(); ++id) require(rig.finish(tickets[id])[0] == id+11, "response ID corruption");
    for (uint32_t id = 0; id < 80; ++id) require(rig.read((id%8)*32)[0] == id%8+11, "reused physical response ID");
}

void range_queue_and_sector_preservation() {
    Rig rig; rig.backing.write(0, 4, 11); rig.backing.write(32, 4, 22);
    rig.read(0); rig.read(32); const auto reads = rig.dram.stats().reads;
    const auto cycle = rig.cache->cycle(); rig.cache->invalidate_range(1, 1);
    rig.backing.write(0, 4, 77); rig.backing.write(32, 4, 88);
    require(rig.cache->cycle() == cycle && !rig.cache->submit({32}), "invalidation bypassed clocked scan");
    require(rig.read(32)[0] == 22 && rig.dram.stats().reads == reads, "neighbor was flushed");
    require(rig.read(0)[0] == 77, "target retained stale bytes");
    for (uint32_t index = 0; index < 64; ++index) rig.cache->invalidate_range(index*32, 1);
    rejects([&] { rig.cache->check_invalidation(4096, 1); }, "cache_invalidation_queue_full");
    rig.cache->invalidate(); require(rig.read(32)[0] == 88, "full flush failed to subsume queued ranges");
}

WarpAccess one(uint64_t address) { auto access = sequence(address, 0); access.mask = 1; return access; }

void hierarchy_coherence_and_live_invalidation() {
    Memory backing; HierarchyConfig config; config.cache_rtl = true;
    HardwareContext context(backing, config); auto& memory = context.memory;
    auto tick = [&] { context.tick(); };
    for (uint32_t sm = 0; sm < 2; ++sm)
        require(await(memory, accepted(memory.submit(sm, one(0))), tick)[0] == 0, "initial bytes");
    auto write = one(0); write.write = true; write.value[0] = 91;
    await(memory, accepted(memory.submit(1, write)), tick);
    require(await(memory, accepted(memory.submit(0, one(0))), tick)[0] == 91, "cross-SM store stale");
    const auto pending = accepted(memory.submit(0, one(4096)));
    rejects([&] { memory.invalidate_range(4097, 1); }, "invalidate_busy_range");
    memory.invalidate_range(32, 4); backing.write(32, 4, 123);
    require(await(memory, pending, tick)[0] == 0, "unrelated read lost during invalidation");
    require(await(memory, accepted(memory.submit(1, one(32))), tick)[0] == 123, "range failed to reach both cache levels");
    require(memory.l1_cache(0).cycle() == context.cycle() && memory.l2_cache().cycle() == context.cycle(), "clocks diverged");
}

// Mixed writes and reads compare returned bytes against an independent byte array under multi-SM traffic.
// 多 SM 混合读写将返回字节与独立字节数组比较。
void randomized_hierarchy_bytes() {
    Memory backing; HierarchyConfig config; config.cache_rtl = true;
    HardwareContext context(backing, config); std::array<uint8_t,1024> expected{};
    std::mt19937 random(104729);
    for (uint32_t index = 0; index < 160; ++index) {
        const auto address = random()%1024; auto access = one(address); access.width = 1;
        access.write = random()%3 == 0; access.value[0] = random()%256;
        auto values = await(context.memory, accepted(context.memory.submit(random()%2, access)), [&] { context.tick(); });
        if (access.write) expected[address] = uint8_t(access.value[0]);
        else require(values[0] == expected[address], "random cached byte mismatch");
    }
    for (uint32_t byte = 0; byte < expected.size(); ++byte)
        require(backing.read(byte,1) == expected[byte], "masked write damaged backing neighbor");
}

void run_cases() {
    run("RTL cache bridge explicit clock and SRAM", explicit_clock_and_sram);
    run("RTL cache bridge finite response ownership", bounded_responses_and_id_reuse);
    run("RTL cache bridge queued range scans", range_queue_and_sector_preservation);
    run("RTL cache hierarchy coherence and invalidation", hierarchy_coherence_and_live_invalidation);
    run("RTL cache hierarchy independent random bytes", randomized_hierarchy_bytes);
}
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return simt::hw::test::run_application(); }

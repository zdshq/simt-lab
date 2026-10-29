#include "hardware_support.hpp"
#include <algorithm>

namespace simt::hw::test {
// Exact timing follows an independently hand-derived command sequence with small configured delays.
// 使用小型配置延迟，按独立手算的命令序列验证精确时序。
void dram_row_timing() {
    Memory memory;
    Dram dram(memory, {2, 128, 4, 3, 2, 4, 1});
    auto ticket = accepted(dram.submit(Packet{0}));
    await(dram, ticket, [&] { dram.tick(); });
    require(dram.cycle() == 7, "cold row requires activate, CAS and burst");
    ticket = accepted(dram.submit(Packet{32}));
    await(dram, ticket, [&] { dram.tick(); });
    require(dram.cycle() == 11, "row hit skips activate/precharge");
    ticket = accepted(dram.submit(Packet{256}));
    await(dram, ticket, [&] { dram.tick(); });
    require(dram.cycle() == 22, "row conflict includes precharge and activate");
    require(dram.stats().row_hits == 1 && dram.stats().row_misses == 2, "row counters");
}

void dram_write_visibility_and_capacity() {
    Memory memory;
    Dram dram(memory, {2, 128, 1, 3, 2, 4, 1});
    Packet write{0, true, {}, 2};
    write.data[1] = 57;
    const auto ticket = accepted(dram.submit(write));
    require(memory.read(1, 1) == 0, "write cannot commit at submit");
    for (uint32_t cycle = 0; cycle < 6; ++cycle) dram.tick();
    require(!dram.take(ticket) && memory.read(1, 1) == 0, "write cannot commit early");
    dram.tick();
    require(!dram.submit(Packet{32}), "unconsumed response retains capacity");
    require(dram.take(ticket)->at(1) == 57 && memory.read(0, 4) == 14592, "byte-enable write");
    require(dram.submit(Packet{32}).has_value(), "taking response releases capacity");
}

void cold_hit_and_real_cache_bytes() {
    CacheRig rig;
    rig.memory.write(0, 4, 0x12345678);
    require(rig.read(0)[0] == 0x78 && rig.cache.cycle() == 11, "cold data and fill timing");
    rig.memory.write(0, 4, 0);
    const auto start = rig.cache.cycle();
    require(rig.read(0)[0] == 0x78, "hit must read cached bytes, not backing bytes");
    require(rig.cache.cycle() - start == 2 && rig.dram.stats().reads == 1, "hit avoids DRAM");
    rig.cache.invalidate();
    require(rig.read(0)[0] == 0 && rig.dram.stats().reads == 2, "explicit invalidation exposes backing change");
}

void independent_sector_valid_bits() {
    CacheRig rig;
    rig.memory.write(32, 4, 99);
    rig.read(0);
    require(rig.read(32)[0] == 99, "missing sector in matching tag still requires fill");
    require(rig.cache.stats().line_misses == 1 && rig.cache.stats().sector_misses == 2, "sector miss accounting");
    require(rig.cache.stats().fill_bytes == 64 && rig.dram.stats().reads == 2, "fetch only required sectors");
}

void lru_replacement_and_conflict() {
    CacheRig rig;
    rig.read(0); rig.read(128); rig.read(0); rig.read(256);
    require(rig.cache.stats().evictions == 1, "third line evicts one way");
    rig.read(0);
    require(rig.cache.stats().hits == 2, "recently touched line survives");
    rig.read(128);
    require(rig.dram.stats().reads == 4, "least recently used line misses again");
}

void mshr_merge_and_limit() {
    CacheRig rig({128, 128, 1, 4, 2, 1, 2, 8});
    const auto first = accepted(rig.cache.submit(Packet{0}));
    rig.tick();
    const auto second = accepted(rig.cache.submit(Packet{0}));
    rig.tick();
    require(!rig.cache.submit(Packet{0}), "merge capacity is finite");
    require(!rig.cache.submit(Packet{32}), "different sector requires another MSHR");
    await(rig.cache, first, [&] { rig.tick(); });
    await(rig.cache, second, [&] { rig.tick(); });
    require(rig.cache.stats().merges == 1 && rig.dram.stats().reads == 1, "merged read issues once");
    rig.read(32);
    require(rig.dram.stats().reads == 2, "MSHR released by fill");
}

void reserved_way_and_bank_backpressure() {
    CacheRig rig({128, 128, 1, 1, 2, 4, 4, 8});
    const auto first = accepted(rig.cache.submit(Packet{0}));
    require(!rig.cache.submit(Packet{32}), "one request per bank per clock");
    rig.tick();
    require(!rig.cache.submit(Packet{128}), "cannot evict an in-flight line");
    const auto second = accepted(rig.cache.submit(Packet{32}));
    await(rig.cache, first, [&] { rig.tick(); });
    await(rig.cache, second, [&] { rig.tick(); });
    require(rig.cache.stats().reservation_stalls == 1, "reservation backpressure counted");
    rig.read(128);
    require(rig.cache.stats().evictions == 1, "line replaceable after all fills");
}

void lower_backpressure_preserves_misses() {
    Memory memory;
    Dram dram(memory, {2, 128, 1, 3, 2, 4, 1});
    SectorCache cache(dram, {256, 128, 2, 4, 2, 4, 4, 8});
    const auto first = accepted(cache.submit(Packet{0}));
    const auto second = accepted(cache.submit(Packet{32}));
    const auto tick = [&] { dram.tick(); cache.tick(); };
    await(cache, first, tick); await(cache, second, tick);
    require(cache.stats().lower_stalls > 0, "full lower queue must cause retries");
    require(dram.stats().reads == 2 && cache.miss_count() == 0, "no duplicate or lost lower request");
}

void write_through_and_read_hazard() {
    CacheRig rig;
    rig.read(0);
    Packet packet{0, true, {}, 1}; packet.data[0] = 27;
    const auto write = accepted(rig.cache.submit(packet));
    rig.tick();
    require(!rig.cache.submit(Packet{0}), "read waits for pending same-sector write");
    await(rig.cache, write, [&] { rig.tick(); });
    require(rig.memory.read(0, 4) == 27, "write-through updates backing memory");
    require(rig.read(0)[0] == 27 && rig.dram.stats().reads == 2, "store invalidates rather than allocates");
}

void coalescing_transactions() {
    require(coalesce(sequence(0)).size() == 4, "32 contiguous words require four sectors");
    require(coalesce(sequence(4)).size() == 5, "misaligned warp requires five sectors");
    require(coalesce(sequence(0, 32)).size() == 32, "scattered warp requires 32 sectors");
    require(coalesce(sequence(0, 0)).size() == 1, "broadcast warp requires one sector");
    auto access = sequence(0); access.mask = 0x101;
    require(coalesce(access).size() == 2, "inactive lanes generate no traffic");
    access.mask = 0;
    require(coalesce(access).empty(), "empty mask has no sectors");
}

void coalescing_store_bytes_and_validation() {
    auto access = sequence(0, 0); access.mask = 3; access.write = true;
    access.value[0] = 0x12345678; access.value[1] = 0xABCDEF01;
    const auto packets = coalesce(access);
    require(packets[0].packet.byte_mask == 15 && packets[0].lanes == 3, "store masks");
    require(packets[0].packet.data[0] == 1 && packets[0].packet.data[3] == 0xAB, "deterministic overlapping store");
    access.address[1] = uint64_t{1} << 32;
    require(coalesce(access).size() == 2, "physical coalescing retains high local address bits");
    rejects([&] { validate_access(access, Memory{}); }, "out_of_bounds");
    access.address[1] = UINT64_MAX - 3;
    rejects([&] { coalesce(access); }, "out_of_bounds");
    access.address[1] = 1;
    rejects([&] { coalesce(access); }, "misaligned");
}

void shared_banks_and_broadcast() {
    BankedShared shared;
    auto ticket = accepted(shared.submit(sequence(0)));
    await(shared, ticket, [&] { shared.tick(); });
    require(shared.cycle() == 3 && shared.stats().bank_services == 32, "independent banks serve together");
    const auto start = shared.cycle();
    ticket = accepted(shared.submit(sequence(0, 128)));
    await(shared, ticket, [&] { shared.tick(); });
    require(shared.cycle() - start == 34, "32 words in one bank require 32 service clocks");
    shared.memory.write(0, 4, 19);
    ticket = accepted(shared.submit(sequence(0, 0)));
    const auto result = await(shared, ticket, [&] { shared.tick(); });
    require(std::all_of(result.begin(), result.end(), [](auto value) { return value == 19; }), "broadcast values");
    require(shared.stats().broadcasts == 31, "one read fans out to 32 lanes");
}

void shared_partial_words_and_store_conflicts() {
    BankedShared shared;
    shared.memory.write(0, 4, 0x44332211);
    auto access = sequence(0, 1); access.width = 1; access.mask = 15;
    auto ticket = accepted(shared.submit(access));
    auto result = await(shared, ticket, [&] { shared.tick(); });
    require(result[0] == 0x11 && result[3] == 0x44 && shared.stats().bank_services == 1, "subword broadcast");
    access = sequence(0, 0); access.write = true; access.mask = 3;
    access.value[0] = 7; access.value[1] = 9;
    ticket = accepted(shared.submit(access));
    shared.tick(); require(shared.memory.read(0, 4) == 7, "first conflicting store");
    shared.tick(); require(shared.memory.read(0, 4) == 9, "second conflicting store");
    await(shared, ticket, [&] { shared.tick(); });
}

void hierarchy_shared_l2_and_coherence() {
    Memory memory;
    MemoryHierarchy hierarchy(memory);
    auto access = sequence(0, 0);
    const auto first = accepted(hierarchy.submit(0, access));
    const auto second = accepted(hierarchy.submit(1, access));
    await(hierarchy, first, [&] { hierarchy.tick(); });
    await(hierarchy, second, [&] { hierarchy.tick(); });
    require(hierarchy.dram_stats().reads == 1 && hierarchy.l2_stats().merges == 1, "two L1s share one L2 fill");
    access.write = true; access.mask = 1; access.value[0] = 73;
    const auto write = accepted(hierarchy.submit(0, access));
    const auto read = accepted(hierarchy.submit(1, sequence(0, 0)));
    await(hierarchy, write, [&] { hierarchy.tick(); });
    const auto result = await(hierarchy, read, [&] { hierarchy.tick(); });
    require(result[31] == 73 && memory.read(0, 4) == 73, "younger cross-SM read cannot see stale L1 data");
}

void hierarchy_capacity_and_invalidation() {
    Memory memory;
    HierarchyConfig config; config.capacity = 1;
    MemoryHierarchy hierarchy(memory, config);
    const auto first = accepted(hierarchy.submit(0, sequence(0)));
    require(!hierarchy.submit(1, sequence(0)), "finite warp queue");
    rejects([&] { hierarchy.invalidate(); }, "busy_hierarchy");
    await(hierarchy, first, [&] { hierarchy.tick(); });
    memory.write(0, 4, 99);
    hierarchy.invalidate();
    const auto next = accepted(hierarchy.submit(1, sequence(0)));
    require(await(hierarchy, next, [&] { hierarchy.tick(); })[0] == 99, "host invalidation boundary");
    rejects([&] { hierarchy.submit(2, sequence(0)); }, "invalid_sm");
}

uint32_t random_word(uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

// The byte-vector reference knows nothing about tags, sectors, MSHRs, or timing.
// 字节向量参考不依赖 tag、sector、MSHR 或时序实现。
std::array<uint32_t, 32> reference(std::vector<uint8_t>& bytes, const WarpAccess& access) {
    std::array<uint32_t, 32> result{};
    for (uint32_t lane = 0; lane < 32; ++lane) {
        if (!(access.mask & (uint32_t{1} << lane))) continue;
        for (uint32_t byte = 0; byte < access.width; ++byte) {
            const auto at = access.address[lane] + byte;
            if (access.write) bytes[at] = static_cast<uint8_t>(access.value[lane] >> (8 * byte));
            else result[lane] |= uint32_t{bytes[at]} << (8 * byte);
        }
    }
    return result;
}

WarpAccess random_access(uint32_t& state) {
    WarpAccess access;
    access.mask = random_word(state); access.width = uint32_t{1} << (random_word(state) % 3);
    access.write = random_word(state) % 3 == 0;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        access.address[lane] = (random_word(state) % (2048 / access.width)) * access.width;
        access.value[lane] = random_word(state);
    }
    return access;
}

void randomized_hierarchy_data() {
    Memory memory;
    HierarchyConfig config; config.l1.bytes = 512; config.l2.bytes = 1024;
    MemoryHierarchy hierarchy(memory, config);
    std::vector<uint8_t> bytes(2048);
    uint32_t state = 1234567;
    for (uint32_t batch = 0; batch < 40; ++batch) {
        std::vector<std::pair<Ticket, std::array<uint32_t, 32>>> expected;
        for (uint32_t index = 0; index < 8; ++index) {
            const auto access = random_access(state);
            const auto ticket = accepted(hierarchy.submit(index % 2, access));
            expected.push_back({ticket, reference(bytes, access)});
        }
        for (const auto& [ticket, result] : expected)
            require(await(hierarchy, ticket, [&] { hierarchy.tick(); }) == result, "randomized byte-reference mismatch");
    }
    for (uint32_t at = 0; at < bytes.size(); ++at)
        require(memory.read(at, 1) == bytes[at], "final DRAM bytes differ from reference");
}

void malformed_configs_and_tickets() {
    Memory memory;
    Dram dram(memory);
    rejects([&] { SectorCache cache(dram, {129, 128, 1}); }, "invalid_cache_config");
    rejects([&] { Dram invalid(memory, {0}); }, "invalid_dram_config");
    rejects([&] { dram.submit(Packet{1}); }, "invalid_sector_address");
    rejects([&] { dram.take(999); }, "invalid_memory_ticket");
    BankedShared shared;
    rejects([&] { shared.submit(sequence(65536)); }, "out_of_bounds");
    rejects([&] { shared.take(999); }, "invalid_shared_ticket");
}

void run_tests() {
    run("dram_row_timing", dram_row_timing);
    run("dram_write_visibility_and_capacity", dram_write_visibility_and_capacity);
    run("cold_hit_and_real_cache_bytes", cold_hit_and_real_cache_bytes);
    run("independent_sector_valid_bits", independent_sector_valid_bits);
    run("lru_replacement_and_conflict", lru_replacement_and_conflict);
    run("mshr_merge_and_limit", mshr_merge_and_limit);
    run("reserved_way_and_bank_backpressure", reserved_way_and_bank_backpressure);
    run("lower_backpressure_preserves_misses", lower_backpressure_preserves_misses);
    run("write_through_and_read_hazard", write_through_and_read_hazard);
    run("coalescing_transactions", coalescing_transactions);
    run("coalescing_store_bytes_and_validation", coalescing_store_bytes_and_validation);
    run("shared_banks_and_broadcast", shared_banks_and_broadcast);
    run("shared_partial_words_and_store_conflicts", shared_partial_words_and_store_conflicts);
    run("hierarchy_shared_l2_and_coherence", hierarchy_shared_l2_and_coherence);
    run("hierarchy_capacity_and_invalidation", hierarchy_capacity_and_invalidation);
    run("randomized_hierarchy_data", randomized_hierarchy_data);
    run("malformed_configs_and_tickets", malformed_configs_and_tickets);
}

int run_application() {
    try { run_tests(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return simt::hw::test::run_application(); }

#include "hardware_support.hpp"
#include "simt/hardware_atomic.hpp"
#include "simt/hardware_local.hpp"

namespace simt::hw::test {
WarpAccess adds(uint32_t mask, uint32_t stride = 0) {
    auto access = sequence(0, stride);
    access.mask = mask; access.write = true; access.atomic = 0;
    access.value.fill(1);
    return access;
}

// A two-lane conflict has separately observable read, ALU, write, and response phases.
// 两个 lane 冲突时，可以分别观察读取、ALU、写入及响应阶段。
void shared_atomic_exact_clock() {
    BankedShared shared({65536, 32, 2, 8, 3});
    const auto ticket = accepted(shared.submit(adds(3)));
    for (uint32_t cycle = 0; cycle < 3; ++cycle) shared.tick();
    require(shared.memory.read(0, 4) == 0 && !shared.take(ticket), "no early ALU visibility");
    shared.tick();
    require(shared.memory.read(0, 4) == 1 && !shared.take(ticket), "first write visible after latency");
    const auto values = await(shared, ticket, [&] { shared.tick(); });
    require(shared.cycle() == 10 && values[0] == 0 && values[1] == 1, "serialized lane clocks and old values");
    require(shared.memory.read(0, 4) == 2 && shared.stats().atomic_completed == 2, "both updates committed");
}

void shared_atomic_banks_and_store_exclusion() {
    BankedShared shared({65536, 32, 2, 8, 3});
    const auto atomic = accepted(shared.submit(adds(3, 4)));
    auto store = sequence(0, 0); store.mask = 1; store.write = true; store.value[0] = 99;
    const auto write = accepted(shared.submit(store));
    const auto result = await(shared, atomic, [&] { shared.tick(); });
    require(shared.cycle() == 6 && result[0] == 0 && result[1] == 0, "independent banks overlap");
    await(shared, write, [&] { shared.tick(); });
    require(shared.memory.read(0, 4) == 99 && shared.memory.read(4, 4) == 1, "ordinary store cannot interleave RMW");
}

void shared_atomic_external_owners() {
    BankedShared shared;
    Memory a(256), b(256);
    a.write(0, 4, 7); b.write(0, 4, 90);
    const auto first = accepted(shared.submit(adds(1), &a));
    const auto second = accepted(shared.submit(adds(1), &b));
    require(await(shared, first, [&] { shared.tick(); })[0] == 7, "first CTA old value");
    require(await(shared, second, [&] { shared.tick(); })[0] == 90, "second CTA old value");
    require(a.read(0, 4) == 8 && b.read(0, 4) == 91, "shared bank keeps owner backing");
}

void global_atomic_queue_and_old_values() {
    CacheRig rig;
    GlobalAtomics atomics(rig.cache, {2, 3, 1});
    const auto ticket = accepted(atomics.submit(adds(3)));
    require(!atomics.submit(adds(1)), "finite command capacity");
    for (uint32_t cycle = 0; cycle < 200; ++cycle) { rig.tick(); atomics.tick(); }
    require(!atomics.submit(adds(1)), "unconsumed response owns capacity");
    const auto result = atomics.take(ticket);
    require(result && (*result)[0] == 0 && (*result)[1] == 1, "duplicate lanes each perform RMW");
    require(rig.memory.read(0, 4) == 2 && atomics.stats().completed == 2, "write-through backing and counts");
    require(atomics.submit(adds(1)).has_value(), "take releases capacity");
}

void atomic_cache_coherence() {
    Memory memory;
    MemoryHierarchy hierarchy(memory);
    auto load = sequence(0, 0); load.mask = 1;
    auto tick = [&] { hierarchy.tick(); };
    require(await(hierarchy, accepted(hierarchy.submit(0, load)), tick)[0] == 0, "prime first L1");
    require(await(hierarchy, accepted(hierarchy.submit(1, load)), tick)[0] == 0, "prime second L1");
    const auto first = accepted(hierarchy.submit(0, adds(3)));
    const auto second = accepted(hierarchy.submit(1, adds(3)));
    const auto younger = accepted(hierarchy.submit(1, load));
    const auto a = await(hierarchy, first, tick), b = await(hierarchy, second, tick);
    require(a[0] == 0 && a[1] == 1 && b[0] == 2 && b[1] == 3, "cross-SM serialization");
    require(await(hierarchy, younger, tick)[0] == 4, "younger read cannot observe stale L1");
}

void mixed_atomic_store_order() {
    Memory memory;
    MemoryHierarchy hierarchy(memory);
    auto store = sequence(0, 0); store.mask = 1; store.write = true; store.value[0] = 90;
    const auto a = accepted(hierarchy.submit(0, adds(1)));
    const auto b = accepted(hierarchy.submit(1, store));
    const auto c = accepted(hierarchy.submit(0, adds(1)));
    auto tick = [&] { hierarchy.tick(); };
    require(await(hierarchy, a, tick)[0] == 0, "first RMW precedes store");
    await(hierarchy, b, tick);
    require(await(hierarchy, c, tick)[0] == 90 && memory.read(0, 4) == 91, "later RMW sees completed store");
}

// Physical words are striped, but architectural bytes stay in their actual per-thread memories.
// 物理字交错排列，架构字节仍保存在实际的逐线程存储中。
void local_translation_and_sector_data() {
    Memory global;
    std::vector<Memory> threads(5, Memory{16384});
    LocalAddressSpace physical(global); physical.attach(7, threads);
    for (uint32_t lane = 0; lane < 5; ++lane) threads[lane].write(12, 4, lane + 80);
    WarpAccess access; access.mask = 31;
    for (uint32_t lane = 0; lane < 5; ++lane) access.address[lane] = LocalAddressSpace::translate(7, lane, 12);
    require(coalesce(access).size() == 1, "same-offset local words coalesce");
    MemoryHierarchy hierarchy(physical);
    const auto result = await(hierarchy, accepted(hierarchy.submit(0, access)), [&] { hierarchy.tick(); });
    for (uint32_t lane = 0; lane < 5; ++lane) require(result[lane] == lane + 80, "local bytes reach cache response");
    require(hierarchy.dram_stats().reads == 1, "partial warp only needs one sector");
    rejects([&] { physical.write(LocalAddressSpace::translate(7, 6, 12), 4, 1); }, "inactive_local_lane");
    rejects([&] { physical.read(LocalAddressSpace::translate(8, 0, 0), 4); }, "unmapped_local");
}

void local_write_visibility_and_global_isolation() {
    Memory global; global.write(0, 4, 777);
    std::vector<Memory> threads(32, Memory{16384});
    LocalAddressSpace physical(global); physical.attach(0, threads);
    MemoryHierarchy hierarchy(physical);
    auto access = sequence(LocalAddressSpace::base); access.write = true;
    for (uint32_t lane = 0; lane < 32; ++lane) access.value[lane] = lane + 1;
    const auto ticket = accepted(hierarchy.submit(0, access));
    require(threads[0].read(0, 4) == 0, "no local write at admission");
    await(hierarchy, ticket, [&] { hierarchy.tick(); });
    for (uint32_t lane = 0; lane < 32; ++lane) require(threads[lane].read(0, 4) == lane + 1, "decoded write owner");
    require(global.read(0, 4) == 777 && hierarchy.dram_stats().writes == 4, "disjoint global domain and coalescing");
}

struct ExpectedMemory { Ticket ticket; uint32_t value; bool returns_value; };

// Generate independent scalar expectations while submitting a mixed stream under finite capacity.
// 在有限容量下提交混合请求流，同时生成独立标量参考结果。
ExpectedMemory enqueue_random(MemoryHierarchy& hierarchy, std::array<uint32_t, 16>& reference, uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    const auto index = (seed >> 8) % reference.size();
    const auto operation = seed % 4;
    auto access = sequence(index * 4, 0); access.mask = 1; access.value[0] = seed;
    const auto old = reference[index];
    if (operation) access.write = true;
    if (operation == 2) access.atomic = 0;
    if (operation == 3) access.atomic = 5;
    const auto ticket = accepted(hierarchy.submit(seed % 2, access));
    if (operation == 1) reference[index] = seed;
    if (operation == 2) reference[index] += seed;
    if (operation == 3) reference[index] ^= seed;
    return {ticket, old, operation != 1};
}

void randomized_mixed_atomic_stream() {
    Memory memory;
    HierarchyConfig config; config.capacity = 4; config.atomic.capacity = 2;
    MemoryHierarchy hierarchy(memory, config);
    std::array<uint32_t, 16> reference{};
    std::deque<ExpectedMemory> pending;
    uint32_t seed = 341, submitted = 0, completed = 0;
    for (uint32_t cycle = 0; completed < 240 && cycle < 100000; ++cycle) {
        while (submitted < 240 && pending.size() < 4) { pending.push_back(enqueue_random(hierarchy, reference, seed)); ++submitted; }
        hierarchy.tick();
        const auto result = hierarchy.take(pending.front().ticket);
        if (!result) continue;
        if (pending.front().returns_value) require((*result)[0] == pending.front().value, "random returned value");
        pending.pop_front(); ++completed;
    }
    require(completed == 240, "mixed stream makes progress");
    for (uint32_t index = 0; index < reference.size(); ++index)
        require(memory.read(index * 4, 4) == reference[index], "random final bytes");
}

void run_cases() {
    run("shared_atomic_exact_clock", shared_atomic_exact_clock);
    run("shared_atomic_banks_and_store_exclusion", shared_atomic_banks_and_store_exclusion);
    run("shared_atomic_external_owners", shared_atomic_external_owners);
    run("global_atomic_queue_and_old_values", global_atomic_queue_and_old_values);
    run("atomic_cache_coherence", atomic_cache_coherence);
    run("mixed_atomic_store_order", mixed_atomic_store_order);
    run("local_translation_and_sector_data", local_translation_and_sector_data);
    run("local_write_visibility_and_global_isolation", local_write_visibility_and_global_isolation);
    run("randomized_mixed_atomic_stream", randomized_mixed_atomic_stream);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return simt::hw::test::run_application(); }

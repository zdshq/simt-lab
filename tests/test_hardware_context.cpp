#include "hardware_support.hpp"
#include "simt/hardware_context.hpp"

namespace simt::hw::test {
WarpAccess scalar(uint64_t address) {
    auto access = sequence(address, 0); access.mask = 1;
    return access;
}

// Clearing one allocation preserves neighboring warm sectors in the same cache line.
// 清零一个分配时，保留同一缓存行内相邻的热 sector。
void range_invalidation_preserves_neighbors() {
    Memory backing; HardwareContext context(backing, {});
    auto& memory = context.memory;
    auto tick = [&] { context.tick(); };
    for (auto address : {0, 32}) await(memory, accepted(memory.submit(0, scalar(address))), tick);
    const auto reads = memory.dram_stats().reads;
    memory.invalidate_range(0, 4); backing.write(0, 4, 99);
    require(await(memory, accepted(memory.submit(0, scalar(32))), tick)[0] == 0, "neighbor value");
    require(memory.dram_stats().reads == reads, "neighbor remains cached");
    require(await(memory, accepted(memory.submit(1, scalar(0))), tick)[0] == 99, "L2 invalidation exposes new bytes");
    require(await(memory, accepted(memory.submit(0, scalar(0))), tick)[0] == 99, "L1 invalidation exposes new bytes");
}

void invalidation_rejects_overlap_and_preserves_unrelated_fill() {
    Memory backing; HardwareContext context(backing, {});
    const auto ticket = accepted(context.memory.submit(0, scalar(0)));
    rejects([&] { context.memory.invalidate_range(31, 1); }, "invalidate_busy_range");
    context.memory.invalidate_range(32, 4);
    backing.write(32, 4, 71);
    await(context.memory, ticket, [&] { context.tick(); });
    const auto next = accepted(context.memory.submit(0, scalar(32)));
    require(await(context.memory, next, [&] { context.tick(); })[0] == 71, "unrelated fill survives");
}

// Draining one owner never consumes a second engine's retained completion.
// 排空一个所有者时，绝不消费另一个引擎保留的完成响应。
void owner_drain_preserves_other_engine() {
    Memory backing; HardwareContext context(backing, {});
    const auto owner = context.new_owner(), other = context.new_owner();
    auto write = scalar(0); write.write = true; write.value[0] = 17;
    accepted(context.memory.submit(0, write, owner));
    const auto read = accepted(context.memory.submit(1, scalar(0), other));
    require(!context.memory.drain_owner(owner), "accepted write still pending");
    for (uint32_t cycle = 0; cycle < 1000 && !context.memory.drain_owner(owner); ++cycle) context.tick();
    require(context.memory.drain_owner(owner), "fault owner drained");
    require(backing.read(0, 4) == 17, "accepted write was not discarded");
    require(await(context.memory, read, [&] { context.tick(); })[0] == 17, "other completion retained");
}

// Retiring a local owner removes backing pointers while later launches use fresh physical tags.
// 退休 local 所有者会移除底层指针，后续启动使用新的物理 tag。
void local_generations_keep_old_tags_disjoint() {
    Memory backing; HardwareContext context(backing, {});
    const auto first = context.reserve_local(32);
    {
        std::vector<Memory> threads(1, Memory{16384}); threads[0].write(0, 4, 123);
        context.physical.attach(first, threads);
        const auto ticket = accepted(context.memory.submit(0, scalar(context.physical.translate(first, 0, 0))));
        require(await(context.memory, ticket, [&] { context.tick(); })[0] == 123, "first owner value");
        context.physical.detach(first, 32);
    }
    const auto second = context.reserve_local(32);
    std::vector<Memory> threads(1, Memory{16384}); context.physical.attach(second, threads);
    const auto ticket = accepted(context.memory.submit(0, scalar(context.physical.translate(second, 0, 0))));
    require(await(context.memory, ticket, [&] { context.tick(); })[0] == 0, "no reuse of retired bytes");
}

void run_cases() {
    run("range_invalidation_preserves_neighbors", range_invalidation_preserves_neighbors);
    run("invalidation_rejects_overlap_and_preserves_unrelated_fill", invalidation_rejects_overlap_and_preserves_unrelated_fill);
    run("owner_drain_preserves_other_engine", owner_drain_preserves_other_engine);
    run("local_generations_keep_old_tags_disjoint", local_generations_keep_old_tags_disjoint);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return simt::hw::test::run_application(); }

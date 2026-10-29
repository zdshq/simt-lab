#include "cache_bench.hpp"
#include "simt/hardware_memory.hpp"
#include <filesystem>
#include <random>

namespace {
constexpr uint64_t conflict_stride = SIMT_RTL_SETS*128;

void send(CacheBench& bench, uint32_t id, const Packet& packet) {
    for (uint32_t limit = 0; limit < 10000; ++limit) if (bench.submit(id, packet)) return;
    throw std::runtime_error("request timeout id="+std::to_string(id)+" address="+std::to_string(packet.address));
}

void cold_warm_sector_and_cached_bytes() {
    CacheBench b("cache-cold.vcd");
    b.backing.write(0, 4, 123); b.backing.write(32, 4, 71);
    send(b, 1, {0}); require(b.wait(1)[0] == 123, "cold data");
    require(b.reads == 1 && b.dut.misses == 1, "cold transaction count");
    b.backing.write(0, 4, 99);
    send(b, 2, {0}); require(b.wait(2)[0] == 123, "hit bypassed actual SRAM");
    require(b.reads == 1 && b.dut.hits == 1, "hit generated downstream traffic");
    send(b, 3, {32}); require(b.wait(3)[0] == 71 && b.reads == 2, "independent sector validity");
    b.clear(); send(b, 4, {0}); require(b.wait(4)[0] == 99, "invalidation did not expose new bytes");
}

void merge_limit_response_capacity_and_backpressure() {
    CacheBench b("cache-capacity.vcd"); b.send_fill = false; b.take_response = false;
    for (uint32_t id = 0; id < 4; ++id) require(b.submit(id, {0}), "merge admission");
    require(!b.submit(4, {0}), "merge limit ignored");
    for (uint32_t id = 4; id < 8; ++id) require(b.submit(id, {32}), "second merge admission");
    require(!b.submit(8, {64}), "response capacity ignored");
    b.send_fill = true;
    for (uint32_t tick = 0; tick < 80; ++tick) b.step();
    require(!b.submit(8, {64}), "unconsumed responses released credits");
    b.dut.invalidate_all = 1; b.dut.eval();
    require(!b.dut.invalidate_ready, "invalidation accepted while responses owned");
    b.take_response = true;
    for (uint32_t id = 0; id < 8; ++id) b.wait(id);
    require(b.reads == 2 && b.dut.merges == 6, "merged reads duplicated lower transactions");
    b.drain();
}

void locked_ways_and_lru_replacement() {
    CacheBench b("cache-lru.vcd"); b.send_fill = false;
    for (uint32_t way = 0; way < 4; ++way) send(b, way, {way*conflict_stride});
    require(!b.submit(9, {4*conflict_stride}), "full MSHRs admitted request");
    b.send_fill = true; for (uint32_t way = 0; way < 4; ++way) b.wait(way);
    send(b, 10, {0}); b.wait(10);
    send(b, 11, {4*conflict_stride}); b.wait(11);
    const auto reads = b.reads;
    send(b, 12, {0}); b.wait(12); require(b.reads == reads, "MRU evicted");
    send(b, 13, {conflict_stride}); b.wait(13); require(b.reads == reads+1, "LRU victim incorrect");
    require(b.dut.evictions == 2, "replacement count incorrect");
}

void byte_masked_write_order_and_no_allocate() {
    CacheBench b("cache-write.vcd");
    for (uint32_t byte = 0; byte < 32; ++byte) b.backing.write(byte, 1, byte);
    send(b, 1, {0}); b.wait(1);
    Packet p{0, true}; p.byte_mask = 0x80000005; p.data.fill(0xA7);
    b.send_fill = false; send(b, 2, p);
    require(!b.submit(3, {0}), "same-sector read passed pending store");
    require(b.backing.read(0, 1) == 0, "write visible before memory completion");
    b.send_fill = true; b.wait(2); send(b, 3, {0}); const auto data = b.wait(3);
    for (uint32_t byte = 0; byte < 32; ++byte)
        require(data[byte] == ((p.byte_mask >> byte)&1 ? 0xA7 : byte), "masked write changed neighbor");
    require(b.reads == 2 && b.writes == 1, "write failed to invalidate warm sector");
    p.address = 4096; send(b, 4, p); b.wait(4); send(b, 5, {4096}); b.wait(5);
    require(b.reads == 3 && b.writes == 2, "write miss allocated a valid sector");
}

// With free MSHR capacity, a younger completed way must be evicted ahead of older locked ways.
// MSHR 容量可用时，必须先替换较新的已完成 way，而不能替换较旧的锁定 way。
void reservation_overrides_lru_with_free_mshr() {
    CacheBench b("cache-reservations.vcd"); b.delay = 200;
    for (uint32_t way = 0; way < 4; ++way) b.backing.write(way*conflict_stride, 4, way+11);
    for (uint32_t way = 0; way < 3; ++way) send(b, way, {way*conflict_stride});
    while (b.reads < 3 && b.cycle < 30) b.step();
    require(b.reads == 3, "older misses not accepted downstream");
    b.delay = 1; send(b, 3, {3*conflict_stride});
    require(b.wait(3)[0] == 14 && b.cycle < 100, "last way did not complete first");
    send(b, 4, {4*conflict_stride}); b.wait(4);
    for (uint32_t way = 0; way < 3; ++way) require(b.wait(way)[0] == way+11, "locked owner overwritten");
    const auto reads = b.reads;
    for (uint32_t way = 0; way < 3; ++way) { send(b, way+5, {way*conflict_stride}); b.wait(way+5); }
    require(b.reads == reads && b.dut.evictions == 1, "pending line displaced instead of unlocked line: reads="+
            std::to_string(b.reads)+" previous="+std::to_string(reads)+" evictions="+std::to_string(b.dut.evictions));
}

void stalled_downstream_and_reset_recovery() {
    CacheBench b("cache-downstream.vcd"); b.send_memory = false;
    send(b, 1, {0});
    for (uint32_t clock = 0; clock < 30; ++clock) b.step();
    require(b.reads == 0 && b.results.empty(), "downstream stall bypassed");
    b.send_memory = true; b.wait(1); b.drain();
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval();
    require(b.dut.idle && !b.dut.rsp_valid && !b.dut.mem_valid, "reset left ownership");
    b.backing.write(0, 4, 17); send(b, 2, {0}); require(b.wait(2)[0] == 17, "reset retained valid SRAM tag");
}

void high_address_tags_and_hit_pipeline() {
    CacheBench b("cache-high-address.vcd");
    const uint64_t high = uint64_t{1} << 36;
    b.backing.write(0, 4, 19); b.backing.write(high, 4, 93);
    send(b, 1, {0}); require(b.wait(1)[0] == 19, "low tag data");
    send(b, 2, {high}); require(b.wait(2)[0] == 93, "upper address bits lost");
    const auto start = b.cycle; b.take_response = false;
    require(b.submit(3, {0}), "warm hit not accepted");
    for (uint32_t cycle = 0; cycle < 2; ++cycle) { require(!b.dut.rsp_valid, "hit returned before SRAM pipeline"); b.step(); }
    require(!b.dut.rsp_valid, "response selection skipped clock"); b.step();
    require(b.dut.rsp_valid && b.cycle == start+4, "hit pipeline clock mismatch");
    b.take_response = true; require(b.wait(3)[0] == 19, "high tag aliased low tag");
}

void out_of_order_fills_and_stalled_payloads() {
    CacheBench b("cache-reorder.vcd"); b.random_stalls = true;
    for (uint32_t id = 0; id < 80; ++id) b.backing.write(id*32, 4, id+1);
    uint32_t submitted = 0, complete = 0;
    for (uint32_t limit = 0; complete < 80 && limit < 10000; ++limit) {
        if (submitted < 80) { if (b.submit(submitted, {submitted*32})) ++submitted; } else b.step();
        for (const auto& [id, data] : b.results) { require(data[0] == id+1, "response routed to wrong ID"); ++complete; }
        b.results.clear();
    }
    require(complete == 80, "reordered stream failed to progress"); b.drain();
}

// Transactional differential testing compares real SRAM results and replacement decisions with the C++ model.
// 事务差分测试将实际 SRAM 结果和替换决策与 C++ 模型比较。
void cpp_model_differential_random_stream() {
    CacheBench b("cache-differential.vcd"); b.random_stalls = true;
    simt::Memory reference;
    simt::hw::Dram dram(reference);
    simt::hw::SectorCache cache(dram, {SIMT_RTL_SETS*4*128, 128, 4, 4, 2, 4, 4, 8});
    std::mt19937 rng(20261029);
    for (uint32_t id = 0; id < 360; ++id) {
        Packet p{(rng()%128)*32, rng()%3 == 0}; p.byte_mask = rng();
        for (auto& byte : p.data) byte = uint8_t(rng());
        const auto ticket = cache.submit(p); require(ticket.has_value(), "C++ sequential backpressure");
        send(b, id, p); const auto actual = b.wait(id);
        std::optional<Sector> expected;
        for (uint32_t limit = 0; !expected && limit < 10000; ++limit) {
            dram.tick(); cache.tick(); expected = cache.take(*ticket);
        }
        require(expected.has_value(), "C++ memory timeout");
        if (!p.write) require(actual == *expected, "RTL and C++ returned different bytes");
        require(b.dut.hits == cache.stats().hits && b.dut.misses == cache.stats().sector_misses &&
                b.dut.evictions == cache.stats().evictions, "RTL and C++ tag decisions differ");
    }
    for (uint32_t byte = 0; byte < 4096; ++byte)
        require(b.backing.read(byte, 1) == reference.read(byte, 1), "final backing bytes differ");
}

// An unaligned range clears intersecting sectors and retains neighboring data in the same line.
// 非对齐范围清除相交 sector，同时保留同一行中的相邻数据。
void range_invalidation_and_scan_clocks() {
    CacheBench b("cache-invalidate-range.vcd");
    b.backing.write(0, 4, 11); b.backing.write(32, 4, 22); b.backing.write(64, 4, 33);
    for (uint32_t id = 0; id < 3; ++id) { send(b, id, {id*32}); b.wait(id); }
    b.drain(); const auto reads = b.reads;
    b.backing.write(0, 4, 77); b.backing.write(32, 4, 88); b.backing.write(64, 4, 99);
    b.dut.invalidate_all = 0; b.dut.invalidate_first = 0; b.dut.invalidate_last = 1;
    b.dut.invalidate_valid = 1; b.dut.eval(); require(b.dut.invalidate_ready, "range refused");
    b.step(); b.dut.invalidate_valid = 0; b.dut.eval();
    require(b.dut.invalidate_busy && !b.dut.req_ready, "scan did not block admission");
    const auto start = b.cycle;
    while (b.dut.invalidate_busy && b.cycle-start < SIMT_RTL_SETS*4+1) b.step();
    require(b.dut.invalidate_done && b.cycle-start == SIMT_RTL_SETS*4, "scan must visit one line per clock");
    send(b, 3, {64}); require(b.wait(3)[0] == 33 && b.reads == reads, "unrelated sector was discarded");
    send(b, 4, {0}); require(b.wait(4)[0] == 77, "first sector stale");
    send(b, 5, {32}); require(b.wait(5)[0] == 88, "last sector stale");
}

void range_invalidation_with_pending_fill() {
    CacheBench b("cache-invalidate-fill.vcd");
    b.backing.write(32, 4, 22); send(b, 0, {32}); b.wait(0); b.drain();
    b.backing.write(32, 4, 55); b.send_fill = false;
    b.backing.write(0, 4, 41); send(b, 1, {0});
    for (uint32_t clock = 0; clock < 30; ++clock) b.step();
    b.dut.invalidate_all = 0; b.dut.invalidate_first = 0; b.dut.invalidate_last = 0;
    b.dut.invalidate_valid = 1; b.dut.eval();
    require(!b.dut.invalidate_ready, "overlapping MSHR allowed invalidation");
    b.dut.invalidate_first = 1; b.dut.invalidate_last = 1; b.dut.eval();
    require(b.dut.invalidate_ready, "unrelated MSHR blocked range");
    b.step(); b.dut.invalidate_valid = 0; b.dut.eval(); b.send_fill = true;
    require(b.wait(1)[0] == 41, "unrelated fill lost"); b.drain();
    const auto reads = b.reads; send(b, 2, {0});
    require(b.wait(2)[0] == 41 && b.reads == reads, "scan damaged concurrent fill");
    send(b, 3, {32}); require(b.wait(3)[0] == 55 && b.reads == reads+1, "same-line fill cancelled invalidation");
}

void run_cases() {
    run_case("RTL cache cold/warm, sector validity, actual SRAM bytes", cold_warm_sector_and_cached_bytes);
    run_case("RTL cache merge/response capacity and backpressure", merge_limit_response_capacity_and_backpressure);
    run_case("RTL cache way reservation and LRU", locked_ways_and_lru_replacement);
    run_case("RTL cache byte writes, serialization, no-write-allocate", byte_masked_write_order_and_no_allocate);
    run_case("RTL cache locked-way replacement with free MSHR", reservation_overrides_lru_with_free_mshr);
    run_case("RTL cache stalled downstream and drained reset", stalled_downstream_and_reset_recovery);
    run_case("RTL cache 64-bit tags and exact SRAM hit pipeline", high_address_tags_and_hit_pipeline);
    run_case("RTL cache out-of-order fills and stable stalled payloads", out_of_order_fills_and_stalled_payloads);
    run_case("RTL cache 360-request C++ differential stream", cpp_model_differential_random_stream);
    run_case("RTL cache sector range and exact metadata scan", range_invalidation_and_scan_clocks);
    run_case("RTL cache range conflict and unrelated in-flight fill", range_invalidation_with_pending_fill);
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return run_application(); }

#include "bench.hpp"
#include "Voperand_collector.h"
#include <array>
#include <bit>
#include <random>
#include <type_traits>
#include <vector>

namespace {
constexpr uint32_t count = COLLECTOR_ENTRIES, index_bits = std::countr_zero(count);
using Vector = std::array<uint32_t,32>;
struct Source { uint32_t reg, mask; Vector values{}; bool waiting = true; };
struct Entry { bool occupied = false; uint64_t age = 0; uint32_t warp = 0, pipe = 0; std::vector<Source> sources; };
class TestBench : public Bench<Voperand_collector> {
public:
    explicit TestBench(const char* name) : Bench(name) {
        dut.allocate_pipe = 0; dut.dispatch_enable = 0; dut.dispatch_accept = 0;
        dut.entry_limit = count; dut.port_groups = 4; dut.eval();
    }
};

template<class Bits> bool bit(const Bits& bits, uint32_t index) {
    if constexpr (std::is_integral_v<Bits>) return (bits >> index) & 1;
    else return (bits[index/32] >> (index%32)) & 1;
}

void commit(TestBench& b) {
    b.tick(); b.dut.collect_enable = 0; b.dut.allocate_valid = 0; b.dut.release_entries = 0; b.dut.dispatch_accept = 0; b.dut.dispatch_enable = 0; b.dut.eval();
}

uint32_t allocate(TestBench& b, uint32_t warp, const std::vector<Source>& sources, uint32_t pipe = 0) {
    b.dut.allocate_pipe = pipe; b.dut.allocate_warp = warp; b.dut.allocate_count = sources.size(); b.dut.allocate_registers = 0;
    for (uint32_t source = 0; source < sources.size(); ++source) {
        b.dut.allocate_registers |= sources[source].reg << (source*6);
        b.dut.allocate_masks[source] = sources[source].mask;
    }
    b.dut.allocate_valid = 1; b.dut.eval();
    require(b.dut.allocate_ready && !b.dut.allocate_error, "legal allocation refused");
    return b.dut.allocate_index;
}

Vector query(TestBench& b, uint32_t entry, uint32_t source) {
    b.dut.query_entry = entry; b.dut.eval(); Vector result;
    for (uint32_t lane = 0; lane < 32; ++lane) result[lane] = b.dut.query_data[source*32+lane];
    return result;
}

void set_bank(TestBench& b, uint32_t bank, uint32_t seed) {
    for (uint32_t lane = 0; lane < 32; ++lane) b.dut.read_data[bank*32+lane] = seed+lane;
    b.dut.eval();
}

// A collector must retain a captured value after the RF bus changes while another source remains pending.
// 另一源尚未就绪时，寄存器堆总线变化不能改变收集器已捕获的值。
void retention_mask_and_clock() {
    TestBench b("collector-retention.vcd");
    const auto id = allocate(b, 0, {{0,1},{4,0x80000000}}); commit(b);
    require(!b.dut.ready_entries, "source appeared ready without collection");
    b.dut.collect_enable = 1; b.dut.eval(); set_bank(b,0,17);
    require(b.dut.read_valid == 1 && !b.dut.ready_entries, "same-bank sources did not serialize");
    commit(b); b.dut.collect_enable = 1; b.dut.eval(); set_bank(b,0,100);
    require(b.dut.ready_entries & (1u << id), "last source did not forward readiness");
    Vector expected{}; expected[0] = 17;
    require(query(b,id,0) == expected, "old source changed or inactive lanes leaked");
    expected.fill(0); expected[31] = 131;
    require(query(b,id,1) == expected, "new source did not use forwarded data");
    commit(b); set_bank(b,0,999);
    require(query(b,id,1) == expected, "held ready operand followed changing input bus");
    b.dut.release_entries = 1u << id; b.dut.eval(); require(!b.dut.release_error, "ready release rejected");
    commit(b); require(b.dut.occupancy == 0, "released state leaked");
}

void capacity_reuse_and_invalid_descriptors() {
    TestBench b("collector-capacity.vcd"); b.dut.entry_limit = 2;
    const auto first = allocate(b,0,{}); commit(b);
    allocate(b,1,{}); commit(b);
    require(!b.dut.allocate_ready && b.dut.occupancy == 2, "capacity overflow");
    b.dut.release_entries = 1u << first; b.dut.eval();
    require(allocate(b,2,{{3,1}}) == first, "same-cycle release credit not reusable"); commit(b);
    require(!(b.dut.ready_entries & (1u << first)), "reallocated slot inherited readiness");
    b.dut.release_entries = 1u << first; b.dut.eval();
    require(b.dut.release_error == (1u << first), "unready release accepted"); commit(b);
    b.dut.reset = 1; b.tick(); b.dut.reset = 0; b.dut.eval();
    require(b.dut.occupancy == 0 && b.dut.allocate_ready, "reset failed to return all capacity");
    b.dut.allocate_valid = 1; b.dut.allocate_count = 1; b.dut.allocate_registers = 63;
    b.dut.allocate_masks[0] = 1; b.dut.eval();
    require(b.dut.allocate_error && !b.dut.allocate_ready, "R63 descriptor accepted");
    b.dut.allocate_registers = 2; b.dut.allocate_masks[0] = 0; b.dut.eval();
    require(b.dut.allocate_error, "empty source mask accepted");
    b.dut.allocate_count = 2; b.dut.allocate_registers = 2 | (2<<6);
    b.dut.allocate_masks[0] = b.dut.allocate_masks[1] = 1; b.dut.eval();
    require(b.dut.allocate_error, "duplicate source not rejected"); commit(b);
    require(b.dut.occupancy == 0, "invalid descriptor mutated occupied state");
}

void age_order_and_port_groups() {
    TestBench b("collector-age.vcd"); b.dut.port_groups = 1;
    const auto first = allocate(b,0,{{3,1},{4,1}}); commit(b);
    const auto second = allocate(b,1,{{0,1}}); commit(b);
    b.dut.collect_enable = 1; b.dut.eval();
    require(b.dut.read_valid == 8 && (b.dut.grant_entries >> (3*index_bits)) == first, "oldest source order differs");
    commit(b); b.dut.collect_enable = 1; b.dut.eval();
    require(b.dut.read_valid == 1 && (b.dut.grant_entries & (count-1)) == first, "oldest remaining source lost priority");
    b.dut.release_entries = 1u << first; b.dut.eval();
    allocate(b,2,{{0,1}}); commit(b); b.dut.collect_enable = 1; b.dut.eval();
    require((b.dut.grant_entries & (count-1)) == second, "reused low index incorrectly became oldest");
}

// Fill every physical entry and repeatedly reuse it while all other entries retain their relative age.
// 填满全部物理条目并反复复用，同时其余条目保持相对年龄。
void full_physical_capacity_and_rank_compaction() {
    TestBench b(nullptr); std::vector<uint32_t> order;
    for (uint32_t index = 0; index < count; ++index) {
        order.push_back(allocate(b,index%8,{{0,UINT32_MAX}})); commit(b);
    }
    require(b.dut.occupancy == count && !b.dut.allocate_ready, "physical capacity not reached");
    b.dut.allocate_valid = 1; b.dut.eval(); commit(b);
    require(b.dut.occupancy == count, "full allocation corrupted count");
    for (uint32_t round = 0; round < count*4; ++round) {
        const auto expected = order.front(); order.erase(order.begin());
        b.dut.collect_enable = 1; b.dut.eval(); set_bank(b,0,round*32);
        require(b.dut.read_valid == 1 && (b.dut.grant_entries & (count-1)) == expected,
                "full-table age rank compacted incorrectly");
        require(query(b,expected,0)[31] == round*32+31, "high entry query selected wrong payload");
        b.dut.release_entries = 1u << expected; b.dut.eval();
        require(allocate(b,round%8,{{0,UINT32_MAX}}) == expected, "full-table same-cycle reuse lost entry");
        order.push_back(expected); commit(b);
    }
}

bool ready(const Entry& entry) {
    if (!entry.occupied) return false;
    for (const auto& source : entry.sources) if (source.waiting) return false;
    return true;
}

// The oracle uses unbounded software ages, independently of the circuit's compact relative ranks.
// 参考使用不回绕的软件年龄，与电路压缩相对排名保持独立。
void service(TestBench& b, std::vector<Entry>& entries, uint32_t cycle) {
    b.dut.collect_enable = 1; b.dut.eval(); uint32_t expected_grants = 0;
    for (uint32_t group = 0; group < b.dut.port_groups; ++group) {
        uint32_t chosen = count, operand = 0; uint64_t age = UINT64_MAX;
        for (uint32_t entry = 0; entry < count; ++entry) if (entries[entry].occupied)
            for (uint32_t source = 0; source < entries[entry].sources.size(); ++source) {
                const auto& candidate = entries[entry].sources[source];
                if (candidate.waiting && candidate.reg%b.dut.port_groups == group && entries[entry].age < age) {
                    age = entries[entry].age; chosen = entry; operand = source;
                }
            }
        if (chosen == count) continue;
        auto& source = entries[chosen].sources[operand]; const auto bank = source.reg%4;
        expected_grants |= 1u << bank;
        require(((b.dut.grant_entries >> (bank*index_bits)) & (count-1)) == chosen, "random age grant differs");
        require(((b.dut.grant_sources >> (bank*2)) & 3) == operand, "random source grant differs");
        require(((b.dut.read_rows >> (bank*7)) & 127) == entries[chosen].warp*16+source.reg/4, "random RF row differs");
        require(b.dut.grant_masks[bank] == source.mask, "grant lane mask differs");
        set_bank(b,bank,cycle*64+bank*32);
        for (uint32_t lane = 0; lane < 32; ++lane)
            source.values[lane] = (source.mask & (1u<<lane)) ? cycle*64+bank*32+lane : 0;
        source.waiting = false;
    }
    require(b.dut.read_valid == expected_grants, "random bank grants differ");
}

void random_lifetimes_and_reuse() {
    TestBench b(nullptr); std::vector<Entry> entries(count); std::mt19937 random(20261101);
    for (uint32_t cycle = 0; cycle < 1200; ++cycle) {
        b.dut.port_groups = 1u << (random()%3); service(b,entries,cycle);
        for (uint32_t entry = 0; entry < count; ++entry) {
            require(bit(b.dut.ready_entries,entry) == ready(entries[entry]), "random ready bits differ");
            if (!ready(entries[entry])) continue;
            for (uint32_t source = 0; source < entries[entry].sources.size(); ++source)
                require(query(b,entry,source) == entries[entry].sources[source].values, "random retained vector differs");
            if (random()%3 == 0) { b.dut.release_entries |= 1u << entry; entries[entry].occupied = false; }
        }
        b.dut.eval(); require(!b.dut.release_error, "random ready release rejected");
        if (b.dut.allocate_ready && random()%4 != 0) {
            Entry entry; entry.occupied = true; entry.age = cycle; entry.warp = random()%8;
            const uint32_t size = random()%4, base = random()%60;
            for (uint32_t source = 0; source < size; ++source) entry.sources.push_back({base+source,uint32_t(random())|1u});
            const auto index = allocate(b,entry.warp,entry.sources);
            require(!entries[index].occupied, "allocation overwrote live entry"); entries[index] = entry;
        }
        commit(b);
        uint32_t used = 0;
        for (uint32_t entry = 0; entry < count; ++entry) {
            used += entries[entry].occupied;
            for (uint32_t source = 0; source < 3; ++source)
                require(bit(b.dut.waiting_sources,entry*3+source) == (entries[entry].occupied &&
                    source < entries[entry].sources.size() && entries[entry].sources[source].waiting), "pending source state differs");
        }
        require(b.dut.occupancy == used, "random occupied count differs");
    }
}

uint32_t dispatch_entry(const TestBench& b, uint32_t port) {
    return (b.dut.dispatch_entries >> (port*index_bits)) & (count-1);
}

void physical_ports_and_acceptance() {
    TestBench b(nullptr); const auto ports = std::min(count,7u);
    std::array<uint32_t,7> ids{};
    for (uint32_t port = 0; port < ports; ++port) {
        ids[port] = allocate(b,port%8,{{port,1u<<port}},port); commit(b);
    }
    for (uint32_t round = 0; round < 2; ++round) {
        b.dut.collect_enable = 1; b.dut.eval();
        for (uint32_t bank = 0; bank < 4; ++bank) set_bank(b,bank,round*100+bank*32);
        commit(b);
    }
    b.dut.dispatch_enable = 1; b.dut.dispatch_eligible = (uint32_t{1}<<(count-1))-1u;
    b.dut.dispatch_eligible |= uint32_t{1}<<(count-1); b.dut.eval();
    require(b.dut.dispatch_valid == (1u<<ports)-1, "physical independent ports missing");
    for (uint32_t port = 0; port < ports; ++port) {
        require(dispatch_entry(b,port) == ids[port], "port selected wrong collector");
        const auto expected = port/4*100+port%4*32+port;
        for (uint32_t lane = 0; lane < 32; ++lane)
            require(b.dut.dispatch_data[port*96+lane] == (lane == port ? expected : 0), "physical output lane data differs");
    }
    const auto before = b.dut.dispatch_entries;
    b.dut.dispatch_accept = (1u<<ports)-1; b.dut.eval();
    require(!b.dut.dispatch_error && b.dut.dispatch_entries == before, "ack changed same-cycle ownership");
    require(std::popcount(uint32_t{b.dut.dispatch_releases}) == ports, "parallel acknowledgements lost");
    require(b.dut.allocate_ready, "ack failed to return same-cycle capacity"); commit(b);
    require(!b.dut.occupancy, "acknowledged entries remained occupied");
    b.dut.dispatch_accept = 1; b.dut.eval(); require(b.dut.dispatch_error == 1, "invalid port ack accepted"); commit(b);
    b.dut.allocate_valid = 1; b.dut.allocate_pipe = 7; b.dut.eval();
    require(b.dut.allocate_error && !b.dut.allocate_ready, "invalid execution pipe accepted");
}

// Dispatch uses independent unbounded ages and verifies payloads before acknowledging random ports.
// 分派参考采用独立不回绕年龄，并在随机确认端口前验证载荷。
void random_dispatch_lifetimes() {
    TestBench b(nullptr); std::vector<Entry> entries(count); std::mt19937 random(20261102);
    for (uint32_t cycle = 0; cycle < 1500; ++cycle) {
        service(b,entries,cycle); b.dut.dispatch_enable = 1; b.dut.dispatch_eligible = random(); b.dut.eval();
        uint32_t valid = 0;
        for (uint32_t port = 0; port < 7; ++port) {
            uint32_t chosen = count; uint64_t age = UINT64_MAX;
            for (uint32_t index = 0; index < count; ++index)
                if (ready(entries[index]) && entries[index].pipe == port && (b.dut.dispatch_eligible & (1u<<index)) && entries[index].age < age) {
                    chosen = index; age = entries[index].age;
                }
            if (chosen == count) continue;
            valid |= 1u<<port; require(dispatch_entry(b,port) == chosen, "random dispatch age differs");
            for (uint32_t source = 0; source < entries[chosen].sources.size(); ++source)
                for (uint32_t lane = 0; lane < 32; ++lane)
                    require(b.dut.dispatch_data[port*96+source*32+lane] == entries[chosen].sources[source].values[lane],
                            "random execution payload differs from collected value");
            if (random()%3 != 0) { b.dut.dispatch_accept |= 1u<<port; entries[chosen].occupied = false; }
        }
        require(b.dut.dispatch_valid == valid, "random execution valid mask differs");
        b.dut.eval(); require(!b.dut.dispatch_error, "random valid ack rejected");
        if (b.dut.allocate_ready && random()%5 != 0) {
            Entry entry; entry.occupied = true; entry.age = cycle; entry.warp = random()%8; entry.pipe = random()%7;
            const auto size = random()%4, base = random()%60;
            for (uint32_t source = 0; source < size; ++source) entry.sources.push_back({base+source,uint32_t(random())|1u});
            const auto index = allocate(b,entry.warp,entry.sources,entry.pipe);
            require(!entries[index].occupied, "dispatch reuse overwrote live entry"); entries[index] = entry;
        }
        commit(b); uint32_t occupied = 0;
        for (const auto& entry : entries) occupied += entry.occupied;
        require(b.dut.occupancy == occupied, "random dispatch capacity differs");
    }
}

void run_cases() {
    run_case("collector physical execution ports and acceptance",physical_ports_and_acceptance);
    run_case("collector 1500-cycle independent dispatch differential",random_dispatch_lifetimes);
    run_case("collector masks retained values and edge ownership",retention_mask_and_clock);
    run_case("collector finite capacity reuse reset and invalid descriptors",capacity_reuse_and_invalid_descriptors);
    run_case("collector age order and shared bank arbitration",age_order_and_port_groups);
    run_case("collector full physical capacity and rank compaction",full_physical_capacity_and_rank_compaction);
    run_case("collector 1200-cycle independent lifetime differential",random_lifetimes_and_reuse);
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

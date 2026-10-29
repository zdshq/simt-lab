#pragma once
#include "simt/memory.hpp"
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace simt::hw {
using Ticket = uint64_t;
constexpr uint32_t sector_bytes = 32;
using Sector = std::array<uint8_t, sector_bytes>;
struct Packet {
    uint64_t address = 0;
    bool write = false;
    Sector data{};
    uint32_t byte_mask = UINT32_MAX;
};
struct Completion { uint64_t due = UINT64_MAX; Sector data{}; };

// Every accepted transaction owns response capacity until its consumer takes the result.
// 每个已接受事务占用响应容量，直到消费者取走结果。
class SectorPort {
public:
    virtual ~SectorPort() = default;
    virtual std::optional<Ticket> submit(const Packet& packet) = 0;
    virtual void tick() = 0;
    std::optional<Sector> take(Ticket ticket);
    uint64_t cycle() const { return cycle_; }
    size_t outstanding() const { return responses_.size(); }
protected:
    uint64_t cycle_ = 0;
    Ticket next_ = 1;
    std::map<Ticket, Completion> responses_;
    Ticket reserve();
    void complete(Ticket ticket, const Sector& data, uint64_t delay = 0);
    void check_packet(const Packet& packet) const;
};

struct DramConfig {
    uint32_t banks = 8, row_bytes = 2048, capacity = 64;
    uint32_t rcd = 12, cas = 12, rp = 12, burst = 4;
};
struct DramStats { uint64_t reads = 0, writes = 0, row_hits = 0, row_misses = 0, queue_stalls = 0; };
struct DramBank { std::optional<uint64_t> row; uint64_t available = 0; };
struct DramRequest { Ticket ticket; Packet packet; uint64_t due = 0; };

// Open-page FCFS banks and a shared data bus form an explicit research DRAM controller.
// open-page FCFS bank 与共享数据总线构成显式的研究型 DRAM 控制器。
class Dram final : public SectorPort {
public:
    Dram(MemoryBacking& memory, DramConfig config = {});
    std::optional<Ticket> submit(const Packet& packet) override;
    void tick() override;
    const DramStats& stats() const { return stats_; }
private:
    MemoryBacking& memory_;
    DramConfig config_;
    DramStats stats_;
    std::vector<DramBank> banks_;
    std::deque<DramRequest> waiting_;
    std::vector<DramRequest> active_;
    uint64_t bus_available_ = 0;
    void dispatch();
    void retire();
    Sector transfer(const Packet& packet);
};

struct CacheConfig {
    uint32_t bytes = 65536, line_bytes = 128, ways = 4, banks = 4;
    uint32_t hit_latency = 4, miss_entries = 16, merge_limit = 16, capacity = 128;
};
struct CacheStats {
    uint64_t reads = 0, writes = 0, hits = 0, sector_misses = 0, line_misses = 0;
    uint64_t merges = 0, evictions = 0, port_stalls = 0, mshr_stalls = 0;
    uint64_t reservation_stalls = 0, response_stalls = 0, lower_stalls = 0, fill_bytes = 0, fill_stalls = 0;
};
struct CacheLine {
    std::optional<uint64_t> tag;
    uint64_t touched = 0;
    uint32_t reservations = 0;
    std::vector<bool> valid;
    std::vector<Sector> data;
};
struct MissEntry {
    Packet packet;
    std::optional<size_t> slot;
    std::optional<Ticket> downstream;
    std::vector<Ticket> waiters;
    uint64_t eligible = 0;
    std::optional<Sector> returned;
};

// Sector valid bits, actual data arrays, locked victims, and merged misses are architectural state.
// sector 有效位、真实数据阵列、被锁定替换行和合并未命中均为模型状态。
class SectorCache final : public SectorPort {
public:
    SectorCache(SectorPort& lower, CacheConfig config = {});
    std::optional<Ticket> submit(const Packet& packet) override;
    void tick() override;
    void invalidate();
    void invalidate_range(uint64_t address, uint64_t size);
    const CacheStats& stats() const { return stats_; }
    size_t miss_count() const { return misses_.size(); }
private:
    SectorPort& lower_;
    CacheConfig config_;
    CacheStats stats_;
    std::vector<CacheLine> lines_;
    std::vector<std::optional<uint64_t>> bank_used_;
    std::vector<std::optional<uint64_t>> fill_used_;
    std::map<uint64_t, MissEntry> misses_;
    uint64_t age_ = 0;
    uint32_t set_count_ = 0;
    size_t sector_index(uint64_t address) const;
    std::optional<size_t> find(uint64_t address) const;
    std::optional<size_t> victim(uint64_t address) const;
    bool available(const Packet& packet);
    Ticket accept(const Packet& packet);
    Ticket hit(const Packet& packet, size_t slot);
    std::optional<Ticket> merge(const Packet& packet, MissEntry& entry);
    std::optional<Ticket> miss(const Packet& packet, std::optional<size_t> slot);
    void reserve_line(size_t slot, uint64_t address);
    bool service(MissEntry& entry);
    void fill(MissEntry& entry, const Sector& data);
};

struct WarpAccess {
    uint32_t mask = UINT32_MAX, width = 4;
    bool write = false;
    std::array<uint64_t, 32> address{};
    std::array<uint32_t, 32> value{};
    std::optional<uint32_t> atomic{};
    std::array<uint32_t, 32> replacement{};
};
struct Coalesced { Packet packet; uint32_t lanes = 0; };
std::vector<Coalesced> coalesce(const WarpAccess& access);
void validate_access(const WarpAccess& access, const MemoryBacking& memory);

struct AtomicConfig { uint32_t banks = 8, latency = 4, capacity = 8; };
struct AtomicStats {
    uint64_t requests = 0, lanes = 0, completed = 0, bank_stalls = 0, queue_stalls = 0;
    uint64_t read_stalls = 0, write_stalls = 0;
};
class GlobalAtomics;
struct HierarchyConfig {
    uint32_t sms = 2, capacity = 64;
    CacheConfig l1;
    CacheConfig l2{262144, 128, 8, 8, 12, 32, 32, 256};
    DramConfig dram;
    AtomicConfig atomic;
};
struct SectorFlight { size_t index; Ticket ticket; };
struct WarpRequest {
    uint32_t sm;
    WarpAccess access;
    std::vector<Coalesced> packets;
    std::vector<SectorFlight> flights;
    size_t issued = 0;
    bool done = false, invalidated = false;
    std::array<uint32_t, 32> result{};
    std::optional<Ticket> atomic_ticket{};
    uint64_t owner = 0;
    bool bypass_l1 = false;
};

// Per-SM L1s share L2/DRAM; conservative global store ordering provides an explicit coherence rule.
// 每 SM 的 L1 共享 L2/DRAM，保守的全局写顺序提供明确的一致性规则。
class MemoryHierarchy {
public:
    MemoryHierarchy(MemoryBacking& memory, HierarchyConfig config = {});
    ~MemoryHierarchy();
    std::optional<Ticket> submit(uint32_t sm, const WarpAccess& access, uint64_t owner = 0, bool bypass_l1 = false);
    void tick();
    std::optional<std::array<uint32_t, 32>> take(Ticket ticket);
    void invalidate();
    void invalidate_range(uint64_t address, uint64_t size);
    bool drain_owner(uint64_t owner);
    uint64_t cycle() const { return dram_.cycle(); }
    const CacheStats& l1_stats(uint32_t sm) const;
    const CacheStats& l2_stats() const { return l2_.stats(); }
    const DramStats& dram_stats() const { return dram_.stats(); }
    const AtomicStats& atomic_stats() const;
private:
    MemoryBacking& memory_;
    HierarchyConfig config_;
    Dram dram_;
    SectorCache l2_;
    std::vector<std::unique_ptr<SectorCache>> l1_;
    std::unique_ptr<GlobalAtomics> atomics_;
    Ticket next_ = 1;
    std::map<Ticket, WarpRequest> requests_;
    void service_requests();
    void service(WarpRequest& request);
    void service_atomic(WarpRequest& request);
    void collect(WarpRequest& request, SectorPort& port);
    void gather(WarpRequest& request, size_t index, const Sector& data);
    void invalidate_l1();
};

struct SharedConfig { uint32_t bytes = 65536, banks = 32, latency = 2, capacity = 8, atomic_latency = 4; };
struct SharedStats {
    uint64_t bank_services = 0, broadcasts = 0, bank_stalls = 0, queue_stalls = 0;
    uint64_t atomic_started = 0, atomic_completed = 0;
};
struct SharedRequest {
    Ticket ticket;
    WarpAccess access;
    uint32_t remaining;
    uint64_t due = UINT64_MAX;
    std::array<uint32_t, 32> result{};
    Memory* backing = nullptr;
    uint32_t atomic_inflight = 0;
};
struct SharedAtomic { Ticket request; uint32_t lane, old, value; uint64_t due; };

// Shared memory serves one word per bank per clock and broadcasts same-word reads.
// shared 每个 bank 每拍服务一个字，并广播同字读取。
class BankedShared {
public:
    explicit BankedShared(SharedConfig config = {});
    Memory memory;
    std::optional<Ticket> submit(const WarpAccess& access, Memory* backing = nullptr);
    void tick();
    std::optional<std::array<uint32_t, 32>> take(Ticket ticket);
    uint64_t cycle() const { return cycle_; }
    const SharedStats& stats() const { return stats_; }
private:
    SharedConfig config_;
    SharedStats stats_;
    uint64_t cycle_ = 0;
    Ticket next_ = 1;
    std::deque<SharedRequest> requests_;
    std::vector<std::optional<SharedAtomic>> atomics_;
    void service(SharedRequest& request, std::vector<bool>& used);
    void serve_word(SharedRequest& request, uint32_t first);
    void start_atomic(SharedRequest& request, uint32_t lane, uint32_t bank);
    void retire_atomics(std::vector<bool>& used);
};
}

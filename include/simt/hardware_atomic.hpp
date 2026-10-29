#pragma once
#include "simt/hardware_memory.hpp"

namespace simt::hw {
enum class AtomicStage { read, execute, write };
struct AtomicJob {
    Ticket request;
    uint32_t lane, old = 0, value = 0;
    uint64_t due = 0;
    AtomicStage stage = AtomicStage::read;
    std::optional<Ticket> memory{};
};
struct AtomicRequest {
    WarpAccess access;
    uint32_t pending, active = 0;
    std::array<uint32_t, 32> result{};
};

// A bank holds its sector lock from read issue through acknowledged write-through completion.
// bank 从读取发起至 write-through 写入确认期间持有 sector 锁。
class GlobalAtomics {
public:
    GlobalAtomics(SectorPort& l2, AtomicConfig config = {});
    std::optional<Ticket> submit(const WarpAccess& access);
    std::optional<std::array<uint32_t, 32>> take(Ticket ticket);
    void tick();
    const AtomicStats& stats() const { return stats_; }
private:
    SectorPort& l2_;
    AtomicConfig config_;
    AtomicStats stats_;
    uint64_t cycle_ = 0;
    Ticket next_ = 1;
    std::map<Ticket, AtomicRequest> requests_;
    std::vector<std::optional<AtomicJob>> banks_;
    void dispatch();
    bool progress(AtomicJob& job);
    bool read(AtomicJob& job);
    bool write(AtomicJob& job);
};
}

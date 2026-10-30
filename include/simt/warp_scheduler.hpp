#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace simt::hw {
struct SchedulerState {
    uint32_t live=0, boundary=0, memory=0, cursor=0, skipped=0;
    std::array<uint32_t,32> inflight{};
};
struct SchedulerStats {
    uint64_t clocks=0, active_clocks=0, admitted=0, released=0, retired=0, probes=0, issued=0;
    uint64_t guard_stalls=0, dependency_stalls=0, boundary_stalls=0, memory_stalls=0, count_stalls=0;
    uint64_t collector_stalls=0, completion_stalls=0, peak_inflight=0;
};

// Ownership, counters and priority live in RTL; callers supply raw frontend validity and probe results.
// 归属、计数和优先级保存在 RTL；调用方提供原始前端有效位与候选检查结果。
class WarpScheduler {
public:
    virtual ~WarpScheduler() = default;
    virtual void allocate(uint32_t slot) = 0;
    virtual void release(uint32_t slot) = 0;
    virtual void retire(uint32_t slot,bool boundary,bool memory) = 0;
    virtual std::optional<uint32_t> probe(uint32_t runnable,uint32_t policy,bool collector,bool completion) = 0;
    virtual bool decide(bool guard,bool dependencies,bool boundary,bool memory) = 0;
    virtual SchedulerState read() const = 0;
    virtual void tick() = 0;
    virtual const SchedulerStats& stats() const = 0;
};
std::unique_ptr<WarpScheduler> make_warp_scheduler(uint32_t slots);
}

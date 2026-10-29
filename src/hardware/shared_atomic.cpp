#include "simt/hardware_memory.hpp"
#include "simt/atomic.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt::hw {
// A shared atomic reserves the bank across its ALU delay, blocking ordinary and DMA accesses too.
// shared 原子操作在 ALU 延迟期间预约 bank，也阻塞普通访问和 DMA。
void BankedShared::start_atomic(SharedRequest& request, uint32_t lane, uint32_t bank) {
    auto& storage = request.backing ? *request.backing : memory;
    const auto& access = request.access;
    const auto old = storage.read(access.address[lane], 4);
    const auto value = atomic_result(*access.atomic, old, access.value[lane], access.replacement[lane]);
    atomics_[bank] = SharedAtomic{request.ticket, lane, old, value, cycle_ + config_.atomic_latency};
    request.remaining &= ~(uint32_t{1} << lane); ++request.atomic_inflight;
    ++stats_.bank_services; ++stats_.atomic_started;
}

// The write uses the bank for this clock; the normal response delay starts after the last write.
// 写入占用当前拍的 bank，最后一次写入后开始普通响应延迟。
void BankedShared::retire_atomics(std::vector<bool>& used) {
    for (uint32_t bank = 0; bank < atomics_.size(); ++bank) {
        auto& job = atomics_[bank];
        if (!job) continue;
        used[bank] = true;
        if (job->due > cycle_) continue;
        const auto at = std::find_if(requests_.begin(), requests_.end(), [&](const auto& r) { return r.ticket == job->request; });
        if (at == requests_.end()) throw std::runtime_error("missing_shared_atomic_owner");
        auto& storage = at->backing ? *at->backing : memory;
        storage.write(at->access.address[job->lane], 4, job->value);
        at->result[job->lane] = job->old; --at->atomic_inflight;
        stats_.bank_services++; stats_.atomic_completed++;
        if (!at->remaining && !at->atomic_inflight) at->due = cycle_ + config_.latency;
        job.reset();
    }
}
}

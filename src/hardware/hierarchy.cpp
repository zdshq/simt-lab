#include "simt/dram_engine.hpp"
#include "simt/hardware_memory.hpp"
#include "simt/hardware_atomic.hpp"
#include "simt/cache_engine.hpp"
#include <stdexcept>

namespace simt::hw {
MemoryHierarchy::MemoryHierarchy(MemoryBacking& memory, HierarchyConfig config)
    : memory_(memory), config_(config), dram_(make_dram(memory, config.dram, config.dram_rtl)),
      l2_(make_cache(*dram_, config.l2, config.cache_rtl, CacheLevel::l2)) {
    if (!config.sms || config.sms > 64 || !config.capacity) throw std::runtime_error("invalid_hierarchy_config");
    for (uint32_t sm = 0; sm < config.sms; ++sm)
        l1_.push_back(make_cache(*l2_, config.l1, config.cache_rtl, CacheLevel::l1));
    atomics_ = std::make_unique<GlobalAtomics>(*l2_, config.atomic);
}

MemoryHierarchy::~MemoryHierarchy() = default;

const AtomicStats& MemoryHierarchy::atomic_stats() const { return atomics_->stats(); }

// Warp requests retain capacity even when responses are ready but not yet consumed.
// warp 请求即使响应已就绪，也保留容量直到消费者取走结果。
std::optional<Ticket> MemoryHierarchy::submit(uint32_t sm, const WarpAccess& access, uint64_t owner, bool bypass_l1) {
    if (sm >= config_.sms) throw std::runtime_error("invalid_sm_index");
    validate_access(access, memory_);
    if (requests_.size() == config_.capacity) return {};
    const auto ticket = next_++;
    auto packets = access.atomic ? std::vector<Coalesced>{} : coalesce(access);
    WarpRequest request{sm, access, std::move(packets), {}, 0, false, false, {}};
    request.owner = owner; request.bypass_l1 = bypass_l1;
    requests_.emplace(ticket, std::move(request));
    return ticket;
}

// Reconstruct lane values from returned sector bytes rather than rereading functional memory.
// 从返回的 sector 字节重建 lane 值，不重新读取功能内存。
void MemoryHierarchy::gather(WarpRequest& request, size_t index, const Sector& data) {
    if (request.access.write) return;
    const auto& packet = request.packets[index];
    for (uint32_t lane = 0; lane < 32; ++lane) {
        if (!(packet.lanes & (uint32_t{1} << lane))) continue;
        const auto offset = request.access.address[lane] - packet.packet.address;
        uint32_t value = 0;
        for (uint32_t byte = 0; byte < request.access.width; ++byte)
            value |= uint32_t{data[offset + byte]} << (8 * byte);
        request.result[lane] = value;
    }
}

void MemoryHierarchy::collect(WarpRequest& request, SectorPort& port) {
    auto flight = request.flights.begin();
    while (flight != request.flights.end()) {
        const auto data = port.take(flight->ticket);
        if (!data) { ++flight; continue; }
        gather(request, flight->index, *data);
        flight = request.flights.erase(flight);
    }
}

// Writes bypass L1 after draining older requests; younger requests wait for their acknowledgement.
// 写入排空较早请求后绕过 L1，较新请求等待写入确认。
void MemoryHierarchy::service(WarpRequest& request) {
    if (request.access.write && !request.invalidated) {
        invalidate_l1();
        request.invalidated = true;
    }
    auto& port = (request.access.write || request.bypass_l1) ? *l2_ : *l1_[request.sm];
    collect(request, port);
    while (request.issued < request.packets.size()) {
        const auto ticket = port.submit(request.packets[request.issued].packet);
        if (!ticket) break;
        request.flights.push_back({request.issued++, *ticket});
    }
    request.done = request.issued == request.packets.size() && request.flights.empty();
}

void MemoryHierarchy::service_requests() {
    bool older_pending = false, atomic_batch = false;
    for (auto& [ticket, request] : requests_) {
        static_cast<void>(ticket);
        if (request.done) continue;
        if (request.access.atomic) {
            if (older_pending && !atomic_batch) break;
            service_atomic(request); atomic_batch = true;
        } else {
            if (atomic_batch || (request.access.write && older_pending)) break;
            service(request);
        }
        older_pending |= !request.done;
        if (request.access.write && !request.access.atomic) break;
    }
}

// All units advance once in bottom-up order; newly forwarded requests wait for a later clock.
// 所有单元自底向上各推进一次，新转发请求等待后续时钟处理。
void MemoryHierarchy::tick() {
    dram_->tick();
    l2_->tick();
    for (auto& cache : l1_) cache->tick();
    atomics_->tick();
    service_requests();
}

// Consecutive atomics can share bank resources; ordinary traffic waits at the coherence boundary.
// 连续原子请求可共享 bank 资源，普通流量在一致性边界等待。
void MemoryHierarchy::service_atomic(WarpRequest& request) {
    if (!request.invalidated) { invalidate_l1(); request.invalidated = true; }
    if (!request.atomic_ticket) request.atomic_ticket = atomics_->submit(request.access);
    if (!request.atomic_ticket) return;
    const auto result = atomics_->take(*request.atomic_ticket);
    if (result) { request.result = *result; request.done = true; }
}

std::optional<std::array<uint32_t, 32>> MemoryHierarchy::take(Ticket ticket) {
    const auto found = requests_.find(ticket);
    if (found == requests_.end()) throw std::runtime_error("invalid_hierarchy_ticket");
    if (!found->second.done) return {};
    const auto result = found->second.result;
    requests_.erase(found);
    return result;
}

void MemoryHierarchy::invalidate_l1() {
    for (auto& cache : l1_) cache->invalidate();
}

// Host-side memory mutation requires a drained hierarchy and an explicit cache flush boundary.
// 主机侧修改内存需要已排空的层次结构和显式缓存失效边界。
void MemoryHierarchy::invalidate() {
    for (const auto& [ticket, request] : requests_) {
        static_cast<void>(ticket);
        if (!request.done) throw std::runtime_error("invalidate_busy_hierarchy");
    }
    invalidate_l1();
    l2_->invalidate();
}

const CacheStats& MemoryHierarchy::l1_stats(uint32_t sm) const {
    return l1_cache(sm).stats();
}

const CachePort& MemoryHierarchy::l1_cache(uint32_t sm) const {
    if (sm >= l1_.size()) throw std::runtime_error("invalid_sm_index");
    return *l1_[sm];
}
}

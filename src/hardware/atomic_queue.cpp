#include "simt/hardware_atomic.hpp"
#include <bit>
#include <stdexcept>

namespace simt::hw {
GlobalAtomics::GlobalAtomics(SectorPort& l2, AtomicConfig config) : l2_(l2), config_(config) {
    if (!config.banks || config.banks > 64 || !config.latency || !config.capacity || config.capacity > 64)
        throw std::runtime_error("invalid_atomic_config");
    banks_.resize(config.banks);
}

// Response ownership consumes queue capacity until the hierarchy takes the complete old-value vector.
// 响应持有队列容量，直到层次结构取走完整旧值向量。
std::optional<Ticket> GlobalAtomics::submit(const WarpAccess& access) {
    validate_access(access, Memory{UINT64_MAX});
    if (!access.atomic || !access.write || access.width != 4) throw std::runtime_error("invalid_atomic_access");
    if (requests_.size() == config_.capacity) { ++stats_.queue_stalls; return {}; }
    const auto ticket = next_++;
    requests_.emplace(ticket, AtomicRequest{access, access.mask});
    ++stats_.requests; stats_.lanes += std::popcount(access.mask);
    return ticket;
}

std::optional<std::array<uint32_t, 32>> GlobalAtomics::take(Ticket ticket) {
    const auto at = requests_.find(ticket);
    if (at == requests_.end()) throw std::runtime_error("invalid_atomic_ticket");
    if (at->second.pending || at->second.active) return {};
    const auto result = at->second.result;
    requests_.erase(at);
    return result;
}

// Older requests win each bank; duplicate lanes are never coalesced into one arithmetic operation.
// 较早请求优先获得 bank，重复 lane 不会合并成一次算术操作。
void GlobalAtomics::dispatch() {
    for (auto& [ticket, request] : requests_) {
        for (uint32_t lane = 0; lane < 32; ++lane) {
            const auto bit = uint32_t{1} << lane;
            if (!(request.pending & bit)) continue;
            const auto bank = request.access.address[lane] / sector_bytes % config_.banks;
            if (banks_[bank]) { ++stats_.bank_stalls; continue; }
            banks_[bank] = AtomicJob{ticket, lane};
            request.pending &= ~bit; ++request.active;
        }
    }
}

void GlobalAtomics::tick() {
    ++cycle_;
    for (auto& bank : banks_) if (bank && progress(*bank)) bank.reset();
    dispatch();
}
}

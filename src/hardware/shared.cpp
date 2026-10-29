#include "simt/hardware_memory.hpp"
#include <stdexcept>

namespace simt::hw {
BankedShared::BankedShared(SharedConfig config) : memory(config.bytes), config_(config) {
    if (!config.bytes || config.bytes % 4 || !config.banks || config.banks > 1024 ||
        !config.latency || !config.capacity) throw std::runtime_error("invalid_shared_config");
}

std::optional<Ticket> BankedShared::submit(const WarpAccess& access, Memory* backing) {
    validate_access(access, backing ? *backing : memory);
    if (requests_.size() == config_.capacity) { ++stats_.queue_stalls; return {}; }
    const auto ticket = next_++;
    requests_.push_back({ticket, access, access.mask, access.mask ? UINT64_MAX : cycle_ + config_.latency, {}, backing});
    return ticket;
}

// Broadcast includes subword reads from one word; conflicting stores serialize lane by lane.
// 广播包含同字的子字读取，有冲突的写入按 lane 逐个串行化。
void BankedShared::serve_word(SharedRequest& request, uint32_t first) {
    const auto& access = request.access;
    auto& storage = request.backing ? *request.backing : memory;
    const auto word = access.address[first] / 4;
    uint32_t served = 0;
    for (uint32_t lane = first; lane < 32; ++lane) {
        if (!(request.remaining & (uint32_t{1} << lane)) || access.address[lane] / 4 != word) continue;
        if (access.write && lane != first) continue;
        if (access.write) storage.write(access.address[lane], access.width, access.value[lane]);
        else request.result[lane] = storage.read(access.address[lane], access.width);
        request.remaining &= ~(uint32_t{1} << lane);
        ++served;
    }
    ++stats_.bank_services;
    stats_.broadcasts += served - 1;
}

// Oldest requests claim banks first; other requests can use independent banks in the same clock.
// 较早请求优先占用 bank，其他请求可在同拍使用独立 bank。
void BankedShared::service(SharedRequest& request, std::vector<bool>& used) {
    if (!request.remaining) return;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        if (!(request.remaining & (uint32_t{1} << lane))) continue;
        const auto bank = request.access.address[lane] / 4 % config_.banks;
        if (used[bank]) { ++stats_.bank_stalls; continue; }
        used[bank] = true;
        serve_word(request, lane);
    }
    if (!request.remaining) request.due = cycle_ + config_.latency;
}

void BankedShared::tick() {
    ++cycle_;
    std::vector<bool> used(config_.banks);
    for (auto& request : requests_) service(request, used);
}

std::optional<std::array<uint32_t, 32>> BankedShared::take(Ticket ticket) {
    for (auto at = requests_.begin(); at != requests_.end(); ++at) {
        if (at->ticket != ticket) continue;
        if (at->due > cycle_) return {};
        const auto result = at->result;
        requests_.erase(at);
        return result;
    }
    throw std::runtime_error("invalid_shared_ticket");
}
}

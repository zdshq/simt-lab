#include "simt/hardware_memory.hpp"
#include <stdexcept>

namespace simt::hw {
// Taking a response releases capacity; an unknown ticket is never an indefinitely pending job.
// 取走响应释放容量，未知 ticket 不会被当作无限等待的任务。
std::optional<Sector> SectorPort::take(Ticket ticket) {
    const auto found = responses_.find(ticket);
    if (found == responses_.end()) throw std::runtime_error("invalid_memory_ticket");
    if (found->second.due > cycle_) return {};
    const auto data = found->second.data;
    responses_.erase(found);
    return data;
}

Ticket SectorPort::reserve() {
    const auto ticket = next_++;
    responses_.emplace(ticket, Completion{});
    return ticket;
}

void SectorPort::complete(Ticket ticket, const Sector& data, uint64_t delay) {
    responses_.at(ticket) = {cycle_ + delay, data};
}

// Sector packets never cross the existing machine's 32-bit global address domain.
// sector 包不得越过现有机器的 32 位 global 地址域。
void SectorPort::check_packet(const Packet& packet) const {
    if (packet.address % sector_bytes || packet.address > (uint64_t{1} << 32) - sector_bytes)
        throw std::runtime_error("invalid_sector_address");
}
}

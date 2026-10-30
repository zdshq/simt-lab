#include "simt/hardware_memory.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt::hw {
// Timing values define this controller; they are not inferred NVIDIA memory timings.
// 时序值定义本控制器，不代表推断出的 NVIDIA 显存时序。
Dram::Dram(MemoryBacking& memory, DramConfig config) : memory_(memory), config_(config) {
    if (!config.banks || config.banks > 1024 || !config.row_bytes || config.row_bytes % sector_bytes ||
        !config.capacity || !config.rcd || !config.cas || !config.rp || !config.burst)
        throw std::runtime_error("invalid_dram_config");
    banks_.resize(config.banks);
}

// Queue saturation rejects a request without reserving a transaction identifier.
// 队列饱和时拒绝请求，不预留事务编号。
std::optional<Ticket> Dram::submit(const Packet& packet) {
    check_packet(packet);
    if (packet.write) memory_.check_write(packet.address,sector_bytes);
    else memory_.check(packet.address, sector_bytes);
    if (outstanding() == config_.capacity) { ++stats_.queue_stalls; return {}; }
    const auto ticket = reserve();
    waiting_.push_back({ticket, packet, 0});
    if (packet.write) ++stats_.writes; else ++stats_.reads;
    if (!packet.write && memory_.instruction_address(packet.address)) ++stats_.instruction_reads;
    return ticket;
}

// Row addresses interleave by bank; one FCFS command can start per model clock.
// 行地址按 bank 交错，每个模型时钟最多启动一个 FCFS 命令。
void Dram::dispatch() {
    if (waiting_.empty()) return;
    auto request = waiting_.front();
    const auto row_address = request.packet.address / config_.row_bytes;
    auto& bank = banks_[row_address % config_.banks];
    if (bank.available > cycle_) return;
    const auto row = row_address / config_.banks;
    const bool hit = bank.row && *bank.row == row;
    const auto penalty = hit ? 0 : config_.rcd + (bank.row ? config_.rp : 0);
    request.due = std::max(cycle_ + config_.cas + penalty, bus_available_) + config_.burst;
    bank.row = row;
    bank.available = request.due;
    bus_available_ = request.due;
    if (hit) ++stats_.row_hits; else ++stats_.row_misses;
    waiting_.pop_front();
    active_.push_back(request);
}

// Actual backing bytes change on write completion, never on command admission.
// 底层真实字节在写完成时改变，不在命令准入时改变。
Sector Dram::transfer(const Packet& packet) {
    Sector result{};
    for (uint32_t byte = 0; byte < sector_bytes; ++byte) {
        const auto address = packet.address + byte;
        if (packet.write && (packet.byte_mask & (uint32_t{1} << byte)))
            memory_.write(address, 1, packet.data[byte]);
        result[byte] = static_cast<uint8_t>(memory_.read(address, 1));
    }
    return result;
}

void Dram::retire() {
    auto request = active_.begin();
    while (request != active_.end()) {
        if (request->due > cycle_) { ++request; continue; }
        complete(request->ticket, transfer(request->packet));
        request = active_.erase(request);
    }
}

void Dram::tick() {
    ++cycle_;
    retire();
    dispatch();
}
}

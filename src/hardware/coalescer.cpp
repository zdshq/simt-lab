#include "simt/hardware_memory.hpp"
#include <stdexcept>

namespace simt::hw {
// Validate all active lanes before any request can cause a partial memory operation.
// 任何请求产生部分内存操作之前，先验证全部活动 lane。
void validate_access(const WarpAccess& access, const Memory& memory) {
    if (access.width != 1 && access.width != 2 && access.width != 4)
        throw std::runtime_error("invalid_memory_width");
    for (uint32_t lane = 0; lane < 32; ++lane)
        if (access.mask & (uint32_t{1} << lane)) memory.check(access.address[lane], access.width, access.width);
}

namespace {
// Ascending lanes define deterministic byte winners for otherwise racing stores in this model.
// 对本模型中存在竞争的写入，按 lane 升序确定每个字节的最终写入者。
void add_lane(std::map<uint64_t, Coalesced>& packets, const WarpAccess& access, uint32_t lane) {
    for (uint32_t byte = 0; byte < access.width; ++byte) {
        const auto address = access.address[lane] + byte;
        const auto base = address / sector_bytes * sector_bytes;
        auto [at, inserted] = packets.try_emplace(base);
        auto& entry = at->second;
        if (inserted) entry.packet = Packet{base, access.write, {}, 0};
        entry.lanes |= uint32_t{1} << lane;
        const auto offset = address % sector_bytes;
        entry.packet.byte_mask |= uint32_t{1} << offset;
        if (access.write) entry.packet.data[offset] = static_cast<uint8_t>(access.value[lane] >> (8 * byte));
    }
}
}

// Merge lane accesses by 32-byte sector, preserving byte enables and participating masks.
// 按 32 字节 sector 合并 lane 访问，保留字节使能与参与掩码。
std::vector<Coalesced> coalesce(const WarpAccess& access) {
    validate_access(access, Memory{});
    std::map<uint64_t, Coalesced> packets;
    for (uint32_t lane = 0; lane < 32; ++lane)
        if (access.mask & (uint32_t{1} << lane)) add_lane(packets, access, lane);
    std::vector<Coalesced> result;
    for (const auto& [base, packet] : packets) {
        static_cast<void>(base);
        result.push_back(packet);
    }
    return result;
}
}

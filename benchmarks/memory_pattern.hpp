#pragma once
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace calibration {
inline void shuffle(std::vector<uint32_t>& values, uint32_t& state) {
    for (size_t i = values.size()-1; i > 0; --i) {
        state ^= state<<13; state ^= state>>17; state ^= state<<5;
        std::swap(values[i],values[state%(i+1)]);
    }
}
// Locality groups control access order; they do not assert the GPU's actual page size.
// 局部性分组控制访问顺序，并不表示 GPU 的真实页大小。
inline std::vector<uint32_t> sector_order(uint32_t bytes, uint32_t seed, uint32_t group_bytes) {
    std::vector<uint32_t> order(bytes/32);
    std::iota(order.begin(),order.end(),0);
    if (!group_bytes) { shuffle(order,seed); return order; }
    std::vector<uint32_t> groups(bytes/group_bytes), local(group_bytes/32);
    std::iota(groups.begin(),groups.end(),0); shuffle(groups,seed);
    size_t at = 0;
    for (auto group : groups) {
        std::iota(local.begin(),local.end(),0); shuffle(local,seed);
        for (auto sector : local) order[at++] = group*(group_bytes/32)+sector;
    }
    return order;
}
// A shuffled cycle visits every 32-byte sector exactly once before returning to zero.
// 乱序环在返回零之前恰好访问每个 32 字节 sector 一次。
inline std::vector<uint32_t> ring(uint32_t bytes, uint32_t seed, uint32_t group_bytes = 0) {
    const auto order = sector_order(bytes,seed,group_bytes);
    std::vector<uint32_t> values(bytes/4);
    for (size_t i = 0; i < order.size(); ++i) values[order[i]*8] = order[(i+1)%order.size()]*8;
    return values;
}
inline uint32_t follow(const std::vector<uint32_t>& values, uint32_t steps) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < steps; ++i) at = values[at];
    return at;
}
}

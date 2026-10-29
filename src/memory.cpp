#include "simt/memory.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace simt {
// Subtraction avoids overflow when validating the end of an access.
// 通过减法验证访问终点，避免地址加法溢出。
void Memory::check(uint64_t address, uint64_t size, uint32_t alignment) const {
    if (address > limit_ || size > limit_ - address) throw std::runtime_error("memory_out_of_bounds");
    if (!alignment || address % alignment) throw std::runtime_error("misaligned_memory_access");
}

// Unallocated bytes read as zero without allocating a page.
// 未分配字节读取为零，且不会分配新页。
uint8_t Memory::byte(uint64_t address) const {
    const auto page = pages_.find(static_cast<uint32_t>(address / page_size));
    return page == pages_.end() ? 0 : page->second[address % page_size];
}

// Writing zero to absent storage preserves the sparse representation.
// 向不存在的存储写零时保持稀疏表示。
void Memory::put(uint64_t address, uint8_t value) {
    const auto id = static_cast<uint32_t>(address / page_size);
    if (value || pages_.contains(id)) pages_[id][address % page_size] = value;
}

// Assemble little-endian values independently of host byte order.
// 独立于宿主字节序组装小端数值。
uint32_t Memory::read(uint64_t address, uint32_t size) const {
    if (size != 1 && size != 2 && size != 4) throw std::runtime_error("invalid_memory_width");
    check(address, size, size);
    uint32_t value = 0;
    for (uint32_t offset = 0; offset < size; ++offset)
        value |= static_cast<uint32_t>(byte(address + offset)) << (offset * 8);
    return value;
}

// Store only the requested low bytes after validating the whole access.
// 检查整个访问范围后，仅写入所需低位字节。
void Memory::write(uint64_t address, uint32_t size, uint32_t value) {
    if (size != 1 && size != 2 && size != 4) throw std::runtime_error("invalid_memory_width");
    check(address, size, size);
    for (uint32_t offset = 0; offset < size; ++offset)
        put(address + offset, static_cast<uint8_t>(value >> (offset * 8)));
}

// Clear only allocated pages, avoiding work proportional to an untouched address range.
// 只清除已分配页，避免按未写地址范围执行大量工作。
void Memory::clear(uint64_t address, uint64_t size) {
    check(address, size);
    auto page = pages_.lower_bound(static_cast<uint32_t>(address / page_size));
    while (page != pages_.end() && uint64_t{page->first} * page_size < address + size) {
        const uint64_t base = uint64_t{page->first} * page_size;
        const auto begin = std::max(address, base) - base;
        const auto end = std::min(address + size, base + page_size) - base;
        if (begin == 0 && end == page_size) page = pages_.erase(page);
        else { std::fill(page->second.begin() + begin, page->second.begin() + end, 0); ++page; }
    }
}

// Host images start at address zero and may initialize read-only device constants.
// 宿主镜像从零地址开始，也可初始化设备只读常量。
void Memory::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot_open_memory_image: " + path);
    const auto length = input.tellg();
    if (length < 0 || length > (64 << 20)) throw std::runtime_error("invalid_memory_image_size");
    check(0, static_cast<uint64_t>(length));
    input.seekg(0);
    for (uint64_t address = 0; address < static_cast<uint64_t>(length); ++address) {
        char value;
        if (!input.get(value)) throw std::runtime_error("cannot_read_memory_image");
        put(address, static_cast<uint8_t>(value));
    }
}

// Sorted nonzero bytes make untouched memory and cleared memory compare equal.
// 排序后的非零字节使未写内存和清零内存具有相同快照。
void Memory::dump(std::ostream& out) const {
    out << '[';
    bool first = true;
    for (const auto& [id, page] : pages_) {
        for (uint32_t offset = 0; offset < page_size; ++offset) {
            if (!page[offset]) continue;
            if (!first) out << ',';
            first = false;
            out << '[' << uint64_t{id} * page_size + offset << ',' << unsigned{page[offset]} << ']';
        }
    }
    out << ']';
}
}

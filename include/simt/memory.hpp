#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>

namespace simt {
// Sparse pages preserve byte addressing without reserving the entire address range.
// 稀疏页保留字节寻址能力，无需预留整个地址范围。
class Memory {
public:
    explicit Memory(uint64_t limit = uint64_t{1} << 32) : limit_(limit) {}
    void check(uint64_t address, uint64_t size, uint32_t alignment = 1) const;
    uint32_t read(uint64_t address, uint32_t size) const;
    void write(uint64_t address, uint32_t size, uint32_t value);
    void load(const std::string& path);
    void dump(std::ostream& out) const;
private:
    static constexpr uint32_t page_size = 4096;
    uint64_t limit_;
    std::map<uint32_t, std::array<uint8_t, page_size>> pages_;
    uint8_t byte(uint64_t address) const;
    void put(uint64_t address, uint8_t value);
};
}

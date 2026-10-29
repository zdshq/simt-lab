#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>

namespace simt {
// Backing stores expose bytes to memory controllers without imposing a logical address layout.
// 底层存储向内存控制器提供字节，不强制采用某种逻辑地址布局。
class MemoryBacking {
public:
    virtual ~MemoryBacking() = default;
    virtual void check(uint64_t address, uint64_t size, uint32_t alignment = 1) const = 0;
    virtual uint32_t read(uint64_t address, uint32_t size) const = 0;
    virtual void write(uint64_t address, uint32_t size, uint32_t value) = 0;
};

// Sparse pages preserve byte addressing without reserving the entire address range.
// 稀疏页保留字节寻址能力，无需预留整个地址范围。
class Memory : public MemoryBacking {
public:
    explicit Memory(uint64_t limit = uint64_t{1} << 32) : limit_(limit) {}
    void check(uint64_t address, uint64_t size, uint32_t alignment = 1) const override;
    uint32_t read(uint64_t address, uint32_t size) const override;
    void write(uint64_t address, uint32_t size, uint32_t value) override;
    void clear(uint64_t address, uint64_t size);
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

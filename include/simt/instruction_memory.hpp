#pragma once
#include "simt/memory.hpp"
#include <span>
#include <vector>

namespace simt::hw {
using InstructionWords=std::array<uint32_t,4>;

// Code occupies a disjoint read-only physical range; allocation is a loader operation, not a clock.
// 代码占用独立的只读物理区间；分配属于加载器操作，不推进时钟。
class InstructionMemory final : public MemoryBacking {
public:
    static constexpr uint64_t base=uint64_t{1}<<63;
    explicit InstructionMemory(MemoryBacking& data) : data_(data) {}
    uint64_t attach(std::span<const InstructionWords> image);
    void detach(uint64_t address) noexcept { images_.erase(address); }
    void check(uint64_t address,uint64_t size,uint32_t alignment=1) const override;
    void check_write(uint64_t address,uint64_t size,uint32_t alignment=1) const override;
    bool instruction_address(uint64_t address) const override { return address>=base; }
    uint32_t read(uint64_t address,uint32_t size) const override;
    void write(uint64_t address,uint32_t size,uint32_t value) override;
    size_t mappings() const { return images_.size(); }
private:
    struct Image { uint64_t bytes; std::vector<InstructionWords> words; };
    MemoryBacking& data_;
    uint64_t next_=base;
    std::map<uint64_t,Image> images_;
    std::map<uint64_t,Image>::const_iterator locate(uint64_t address) const;
};

// The launch retains this mapping until all accepted requests have drained, including after failure.
// 启动保留此映射，直到全部已接受请求排空，失败路径也遵守此规则。
class InstructionMapping {
public:
    InstructionMapping(InstructionMemory& memory,std::span<const InstructionWords> image)
        : memory_(memory), address_(memory.attach(image)), words_(image.size()) {}
    ~InstructionMapping() { memory_.detach(address_); }
    InstructionMapping(const InstructionMapping&)=delete;
    InstructionMapping& operator=(const InstructionMapping&)=delete;
    uint64_t address() const { return address_; }
    uint64_t words() const { return words_; }
private:
    InstructionMemory& memory_;
    uint64_t address_,words_;
};
}

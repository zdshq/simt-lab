#include "simt/instruction_memory.hpp"
#include <stdexcept>

namespace simt::hw {
uint64_t InstructionMemory::attach(std::span<const InstructionWords> image) {
    if (image.empty() || image.size()>(uint64_t{1}<<32)) throw std::runtime_error("invalid_instruction_image_size");
    const auto bytes=(image.size()*16+63)&~uint64_t{63};
    if (bytes>UINT64_MAX-next_) throw std::runtime_error("instruction_physical_addresses_exhausted");
    const auto address=next_;
    images_.emplace(address,Image{bytes,{image.begin(),image.end()}}); next_+=bytes;
    return address;
}

std::map<uint64_t,InstructionMemory::Image>::const_iterator InstructionMemory::locate(uint64_t address) const {
    auto at=images_.upper_bound(address);
    if (at==images_.begin()) throw std::runtime_error("unmapped_instruction_physical_address");
    --at;
    if (address-at->first>=at->second.bytes) throw std::runtime_error("unmapped_instruction_physical_address");
    return at;
}

void InstructionMemory::check(uint64_t address,uint64_t size,uint32_t alignment) const {
    if (address<base) {
        if (size>base-address) throw std::runtime_error("crossing_instruction_address_space");
        data_.check(address,size,alignment); return;
    }
    if (!alignment || address%alignment) throw std::runtime_error("misaligned_memory_access");
    const auto at=locate(address);
    if (size>at->second.bytes-(address-at->first)) throw std::runtime_error("instruction_mapping_range");
}

// End-of-image padding supplies real zero bytes to complete sector and instruction-line fills.
// 映像末尾填充提供实际零字节，用于补齐 sector 和指令缓存行。
uint32_t InstructionMemory::read(uint64_t address,uint32_t size) const {
    if (address<base) return data_.read(address,size);
    if (size!=1 && size!=2 && size!=4) throw std::runtime_error("invalid_memory_width");
    check(address,size,size); const auto at=locate(address); const auto offset=address-at->first;
    if (offset>=at->second.words.size()*16) return 0;
    const auto word=at->second.words[offset/16][(offset/4)%4];
    const auto value=word>>(8*(offset%4));
    return size==4 ? value : value&((uint32_t{1}<<(8*size))-1);
}

void InstructionMemory::write(uint64_t address,uint32_t size,uint32_t value) {
    check_write(address,size,size); data_.write(address,size,value);
}

// Reject forbidden writes before cache state, tickets or byte transfers can be changed.
// 在修改缓存状态、票据或传输字节前拒绝禁止的写入。
void InstructionMemory::check_write(uint64_t address,uint64_t size,uint32_t alignment) const {
    if (address>=base) throw std::runtime_error("write_to_instruction_memory");
    check(address,size,alignment); data_.check_write(address,size,alignment);
}
}

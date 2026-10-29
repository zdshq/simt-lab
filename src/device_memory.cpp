#include "simt/device.hpp"
#include <stdexcept>

namespace simt {
// First-fit allocation reuses cleared gaps and keeps pointer zero invalid.
// 首次适配分配复用已清零的空隙，并保留零指针为无效值。
uint32_t DeviceArena::allocate(uint32_t size) {
    constexpr uint64_t end = 65536 + (64 << 20);
    if (!size || size > (64 << 20)) throw std::runtime_error("invalid_allocation_size");
    uint64_t address = 65536;
    for (const auto& [base, allocation] : allocations_) {
        if (address + size <= base) break;
        address = (uint64_t{base} + allocation.size + 255) & ~uint64_t{255};
    }
    if (address + size > end) throw std::runtime_error("device_out_of_memory");
    if (before_clear) before_clear(address, size);
    memory->clear(address, size);
    allocations_.emplace(static_cast<uint32_t>(address), Allocation{size, 0});
    return static_cast<uint32_t>(address);
}

// Pending commands retain buffers even before their queue entry is fetched.
// 即使队列项尚未被取走，待执行命令也会保留缓冲区。
void DeviceArena::release(uint32_t address) {
    validate(address);
    const auto allocation = allocations_.at(address);
    if (allocation.pins) throw std::runtime_error("buffer_in_use");
    if (before_clear) before_clear(address, allocation.size);
    memory->clear(address, allocation.size);
    allocations_.erase(address);
}

// Range checks reject copies that cross allocation boundaries.
// 范围检查拒绝跨越分配边界的拷贝。
uint32_t DeviceArena::containing(uint32_t address, uint32_t size) const {
    auto allocation = allocations_.upper_bound(address);
    if (allocation == allocations_.begin()) throw std::runtime_error("invalid_device_pointer");
    --allocation;
    const uint64_t offset = uint64_t{address} - allocation->first;
    if (offset > allocation->second.size || size > allocation->second.size - offset)
        throw std::runtime_error("copy_out_of_allocation");
    return allocation->first;
}

// Kernel resource references use allocation bases rather than ambiguous interior pointers.
// kernel 资源引用使用分配基址，不使用含义不清的内部指针。
void DeviceArena::validate(uint32_t address) const {
    if (!allocations_.contains(address)) throw std::runtime_error("invalid_device_pointer");
}

// Validate all references before incrementing any lifetime counter.
// 增加生命周期计数之前先检查全部引用。
void DeviceArena::pin(const std::set<uint32_t>& addresses) {
    for (auto address : addresses) validate(address);
    for (auto address : addresses) ++allocations_.at(address).pins;
}

// Completion releases exactly the references held by the public submission.
// 完成时准确释放公开提交所持有的引用。
void DeviceArena::unpin(const std::set<uint32_t>& addresses) {
    for (auto address : addresses) --allocations_.at(address).pins;
}

// Monitoring reports allocated device bytes, not host process RSS or cache-model capacity.
// 监控报告已分配的设备字节，不是宿主 RSS 或缓存模型容量。
uint64_t DeviceArena::allocated_bytes() const {
    uint64_t total = 0;
    for (const auto& [address, allocation] : allocations_) {
        static_cast<void>(address);
        total += allocation.size;
    }
    return total;
}
}

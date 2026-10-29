#pragma once
#include "simt/hardware_local.hpp"
#include "simt/hardware_memory.hpp"

namespace simt::hw {
// Device lifetime owns caches and physical decoding; individual kernels own warp and CTA state.
// 设备生命周期持有缓存和物理解码，各 kernel 持有 warp 与 CTA 状态。
class HardwareContext {
public:
    HardwareContext(Memory& global, HierarchyConfig config);
    LocalAddressSpace physical;
    MemoryHierarchy memory;
    uint64_t new_owner();
    uint64_t reserve_local(uint64_t warps);
    void tick() { memory.tick(); }
    uint64_t cycle() const { return memory.cycle(); }
    uint32_t sms() const { return sms_; }
    void dump(std::ostream& out) const;
private:
    uint32_t sms_;
    uint64_t next_owner_ = 1, next_warp_ = 0;
};
}

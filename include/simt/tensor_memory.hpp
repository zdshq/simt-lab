#pragma once
#include "simt/hardware_memory.hpp"
#include "simt/hardware_tensor.hpp"

namespace simt::hw {
struct TmemAllocationStats {
    uint64_t allocated=0, freed=0, reclaimed=0, peak_columns=0;
};

// One SM shares finite columns; attached memories are observations in the RTL backend.
// 一个 SM 共享有限列容量；RTL 后端中的附加内存只作观测。
class TensorMemory : public SharedPort {
public:
    virtual uint32_t allocate(Memory& memory,uint32_t count)=0;
    virtual void free(Memory& memory,uint32_t base,uint32_t count)=0;
    virtual uint32_t coordinate(Memory& memory,uint32_t row,uint32_t column)=0;
    virtual uint32_t columns() const=0;
    virtual bool pending() const=0;
    virtual const TmemAllocationStats& allocations() const=0;
};
std::unique_ptr<TensorMemory> make_tensor_memory(const TensorConfig& config,bool rtl);
}

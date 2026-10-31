#include "simt/tensor_memory.hpp"
#include <algorithm>
#include <bitset>
#include <stdexcept>

namespace simt::hw {
namespace {
struct Columns { std::bitset<256> occupied; std::map<uint32_t,uint32_t> allocations; };
class ModelTensorMemory final : public TensorMemory {
public:
    explicit ModelTensorMemory(const TensorConfig& config)
        : port_({131072,config.tmem_banks,config.tmem_latency,8}), limit_(config.tmem_columns) {}
    void attach(Memory& memory,uint32_t,uint32_t) override;
    void detach(Memory& memory) override;
    uint32_t allocate(Memory& memory,uint32_t count) override;
    void free(Memory& memory,uint32_t base,uint32_t count) override;
    uint32_t coordinate(Memory& memory,uint32_t row,uint32_t column) override;
    std::optional<Ticket> submit(const WarpAccess& access,Memory* memory) override;
    std::optional<std::array<uint32_t,32>> take(Ticket ticket) override;
    void tick() override { port_.tick(); }
    uint64_t cycle() const override { return port_.cycle(); }
    const SharedStats& stats() const override { return port_.stats(); }
    std::string_view name() const override { return "model"; }
    size_t outstanding() const override { return port_.outstanding(); }
    uint32_t columns() const override { return used_; }
    bool pending() const override { return outstanding()!=0; }
    const TmemAllocationStats& allocations() const override { return stats_; }
private:
    BankedShared port_;
    uint32_t limit_,used_=0;
    TmemAllocationStats stats_;
    std::map<Memory*,Columns> owners_;
    std::map<Ticket,Memory*> requests_;
    Columns& owner(Memory* memory);
    void check_idle(Memory& memory) const;
};
Columns& ModelTensorMemory::owner(Memory* memory) {
    const auto found=owners_.find(memory);
    if (found==owners_.end()) throw std::runtime_error("unknown_tensor_memory_owner");
    return found->second;
}
void ModelTensorMemory::check_idle(Memory& memory) const {
    for (const auto& [ticket,backing] : requests_) {
        static_cast<void>(ticket);
        if (backing==&memory) throw std::runtime_error("tensor_memory_owner_busy");
    }
}
void ModelTensorMemory::attach(Memory& memory,uint32_t,uint32_t) {
    if (!owners_.emplace(&memory,Columns{}).second) throw std::runtime_error("duplicate_tensor_memory_owner");
}
void ModelTensorMemory::detach(Memory& memory) {
    auto& state=owner(&memory); check_idle(memory);
    used_-=static_cast<uint32_t>(state.occupied.count()); stats_.reclaimed+=state.occupied.count(); owners_.erase(&memory);
}

// Match the existing eight-column logical allocator while charging one shared SM capacity.
// 保持既有八列逻辑分配规则，同时扣减一份共享 SM 容量。
uint32_t ModelTensorMemory::allocate(Memory& memory,uint32_t count) {
    auto& state=owner(&memory);
    if (!count || count>256 || count%8) throw std::runtime_error("invalid_tensor_allocation");
    if (count>limit_-used_) throw std::runtime_error("tensor_out_of_columns");
    for (uint32_t base=0; base+count<=256; base+=8) {
        bool available=true;
        for (uint32_t column=base; column<base+count; ++column) available&=!state.occupied[column];
        if (!available) continue;
        for (uint32_t column=base; column<base+count; ++column) state.occupied.set(column);
        state.allocations[base]=count; used_+=count; stats_.allocated+=count;
        stats_.peak_columns=std::max(stats_.peak_columns,uint64_t{used_}); return base;
    }
    throw std::runtime_error("tensor_out_of_columns");
}
void ModelTensorMemory::free(Memory& memory,uint32_t base,uint32_t count) {
    auto& state=owner(&memory); check_idle(memory);
    const auto at=state.allocations.find(base);
    if (at==state.allocations.end() || at->second!=count) throw std::runtime_error("invalid_tensor_free");
    for (uint32_t column=base; column<base+count; ++column) state.occupied.reset(column);
    state.allocations.erase(at); used_-=count; stats_.freed+=count;
}
uint32_t ModelTensorMemory::coordinate(Memory& memory,uint32_t row,uint32_t column) {
    if (row>=128 || column>=256) throw std::runtime_error("tensor_coordinate_out_of_range");
    if (!owner(&memory).occupied[column]) throw std::runtime_error("tensor_column_not_allocated");
    return (column*128+row)*4;
}
std::optional<Ticket> ModelTensorMemory::submit(const WarpAccess& access,Memory* memory) {
    owner(memory); validate_access(access,*memory);
    if (access.width!=4 || access.atomic) throw std::runtime_error("invalid_tensor_memory_access");
    for (uint32_t lane=0; lane<32; ++lane) if (access.mask & (1u<<lane))
        coordinate(*memory,static_cast<uint32_t>(access.address[lane]/4%128),static_cast<uint32_t>(access.address[lane]/512));
    const auto ticket=port_.submit(access,memory); if (ticket) requests_[*ticket]=memory; return ticket;
}
std::optional<std::array<uint32_t,32>> ModelTensorMemory::take(Ticket ticket) {
    const auto result=port_.take(ticket); if (result) requests_.erase(ticket); return result;
}
}
#ifdef SIMT_HAS_REGISTER_RTL
std::unique_ptr<TensorMemory> make_rtl_tensor_memory(const TensorConfig& config);
#endif
std::unique_ptr<TensorMemory> make_tensor_memory(const TensorConfig& config,bool rtl) {
    if (!config.tmem_columns || config.tmem_columns>256 || config.tmem_columns%8)
        throw std::runtime_error("invalid_tensor_pipeline_config");
    if (!rtl) return std::make_unique<ModelTensorMemory>(config);
#ifdef SIMT_HAS_REGISTER_RTL
    return make_rtl_tensor_memory(config);
#else
    throw std::runtime_error("tensor_memory_rtl_backend_unavailable");
#endif
}
}

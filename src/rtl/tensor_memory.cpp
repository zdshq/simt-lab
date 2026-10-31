#include "simt/tensor_memory.hpp"
#include "Vsimt_tmem1.h"
#include "Vsimt_tmem2.h"
#include "Vsimt_tmem4.h"
#include "Vsimt_tmem8.h"
#include "Vsimt_tmem16.h"
#include "Vsimt_tmem32.h"
#include "Vsimt_tmem64.h"
#include "Vsimt_tmem128.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <type_traits>

namespace simt::hw {
namespace {
// Packed fields may span words; small parameter geometries use scalar Verilator ports.
// 打包字段可能跨字；小参数配置使用 Verilator 标量端口。
template<class Words> uint32_t field(const Words& words,uint32_t index,uint32_t width) {
    const uint32_t bit=index*width;
    uint64_t value;
    if constexpr (std::is_integral_v<Words>) value=uint64_t{words}>>(bit%64);
    else {
        value=words[bit/32];
        if (bit%32+width>32) value|=uint64_t{words[bit/32+1]}<<32;
        value>>=bit%32;
    }
    return static_cast<uint32_t>(value & ((uint64_t{1}<<width)-1));
}
template<class Words> void put(Words& words,uint32_t index,uint32_t width,uint32_t value) {
    const auto bit=index*width,shift=bit%32;
    words[bit/32]|=value<<shift;
    if (shift+width>32) words[bit/32+1]|=value>>(32-shift);
}
struct Transfer { Ticket ticket; Memory* observation; };
template<class Dut,uint32_t Banks> class RtlTensorMemory final : public TensorMemory {
public:
    explicit RtlTensorMemory(const TensorConfig& config);
    ~RtlTensorMemory() override { dut_.final(); }
    void attach(Memory& memory,uint32_t slot,uint32_t) override;
    void detach(Memory& memory) override;
    uint32_t allocate(Memory& memory,uint32_t count) override;
    void free(Memory& memory,uint32_t base,uint32_t count) override;
    uint32_t coordinate(Memory& memory,uint32_t row,uint32_t column) override;
    std::optional<Ticket> submit(const WarpAccess& access,Memory* backing) override;
    std::optional<std::array<uint32_t,32>> take(Ticket ticket) override;
    void tick() override;
    uint64_t cycle() const override { return stats_.clocks; }
    const SharedStats& stats() const override { return stats_; }
    std::string_view name() const override { return "rtl"; }
    size_t outstanding() const override;
    uint32_t columns() const override { return dut_.used_columns; }
    bool pending() const override { return dut_.command_valid || dut_.retire_ctas || dut_.request_valid || dut_.acknowledge || dut_.occupied; }
    const TmemAllocationStats& allocations() const override { return allocation_stats_; }
private:
    Dut dut_;
    SharedStats stats_;
    TmemAllocationStats allocation_stats_;
    std::array<Memory*,64> observations_{};
    std::array<std::optional<Transfer>,8> transfers_;
    Ticket next_=1;
    uint32_t owner(Memory* memory) const;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void clear();
    void command(Memory& memory,bool allocate,uint32_t base,uint32_t count);
    void drive(const WarpAccess& access,uint32_t owner);
    void sample();
};
const char* command_error(uint32_t error) {
    switch (error) {
    case 2: return "invalid_tensor_allocation";
    case 3: return "tensor_out_of_columns";
    case 4: return "invalid_tensor_free";
    case 5: return "tensor_memory_owner_busy";
    default: return "tensor_memory_control_error";
    }
}
template<class D,uint32_t B> void RtlTensorMemory<D,B>::clear() {
    dut_.command_valid=0; dut_.retire_ctas=0; dut_.request_valid=0; dut_.acknowledge=0;
}
template<class D,uint32_t B> RtlTensorMemory<D,B>::RtlTensorMemory(const TensorConfig& config) {
    if (!config.tmem_latency) throw std::runtime_error("invalid_tensor_pipeline_config");
    clear(); dut_.column_limit=config.tmem_columns; dut_.response_latency=config.tmem_latency;
    dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}
template<class D,uint32_t B> uint32_t RtlTensorMemory<D,B>::owner(Memory* memory) const {
    if (memory) for (uint32_t slot=0; slot<64; ++slot) if (observations_[slot]==memory) return slot;
    throw std::runtime_error("unknown_tensor_memory_owner");
}
template<class D,uint32_t B> void RtlTensorMemory<D,B>::attach(Memory& memory,uint32_t slot,uint32_t) {
    if (slot>=64 || observations_[slot] || std::find(observations_.begin(),observations_.end(),&memory)!=observations_.end())
        throw std::runtime_error("duplicate_tensor_memory_owner");
    memory.check(0,131072); observations_[slot]=&memory;
}
template<class D,uint32_t B> void RtlTensorMemory<D,B>::detach(Memory& memory) {
    const auto slot=owner(&memory); const auto bit=uint64_t{1}<<slot;
    if ((dut_.busy_ctas & bit) || (dut_.command_valid && dut_.command_cta==slot) ||
        (dut_.request_valid && dut_.request_cta==slot)) throw std::runtime_error("tensor_memory_owner_busy");
    dut_.retire_ctas|=bit; dut_.eval();
    if (dut_.error) { dut_.retire_ctas&=~bit; dut_.eval(); throw std::runtime_error("tensor_memory_retirement_error"); }
    observations_[slot]=nullptr;
}

// Keep the circuit's selected logical base and physical pages stable until the explicit edge.
// 将电路选出的逻辑基址和物理页保持到显式边沿。
template<class D,uint32_t B> void RtlTensorMemory<D,B>::command(Memory& memory,bool allocate,uint32_t base,uint32_t count) {
    if (dut_.command_valid) throw std::runtime_error("tensor_memory_control_port_busy");
    dut_.command_cta=owner(&memory); dut_.command_allocate=allocate; dut_.command_base=base; dut_.command_count=count; dut_.eval();
    dut_.held_base=dut_.grant_base; dut_.held_pages=dut_.grant_pages; dut_.command_valid=1; dut_.eval();
    if (!dut_.error) return;
    const auto error=dut_.error; dut_.command_valid=0; dut_.eval(); throw std::runtime_error(command_error(error));
}
template<class D,uint32_t B> uint32_t RtlTensorMemory<D,B>::allocate(Memory& memory,uint32_t count) {
    command(memory,true,0,count); return uint32_t{dut_.grant_base}*8;
}
template<class D,uint32_t B> void RtlTensorMemory<D,B>::free(Memory& memory,uint32_t base,uint32_t count) {
    command(memory,false,base,count);
}
template<class D,uint32_t B> uint32_t RtlTensorMemory<D,B>::coordinate(Memory& memory,uint32_t row,uint32_t column) {
    dut_.coordinate_cta=owner(&memory); dut_.coordinate_row=row; dut_.coordinate_column=column; dut_.eval();
    if (dut_.coordinate_error) throw std::runtime_error(dut_.coordinate_error==1 ? "tensor_coordinate_out_of_range" : "tensor_column_not_allocated");
    return dut_.coordinate_address;
}
template<class D,uint32_t B> void RtlTensorMemory<D,B>::drive(const WarpAccess& access,uint32_t owner_id) {
    dut_.request_write=access.write; dut_.request_cta=owner_id; dut_.request_mask=access.mask;
    for (auto& word : dut_.request_words) word=0;
    for (uint32_t lane=0; lane<32; ++lane) {
        const auto address=(access.mask & (1u<<lane)) ? access.address[lane]/4 : 0;
        put(dut_.request_words,lane,15,static_cast<uint32_t>(address)); dut_.request_values[lane]=access.value[lane];
    }
}
template<class D,uint32_t B> std::optional<Ticket> RtlTensorMemory<D,B>::submit(const WarpAccess& access,Memory* backing) {
    const auto owner_id=owner(backing); validate_access(access,*backing);
    if (access.width!=4 || access.atomic) throw std::runtime_error("invalid_tensor_memory_access");
    for (uint32_t lane=0; lane<32; ++lane) if ((access.mask & (1u<<lane)) && access.address[lane]>=131072)
        throw std::runtime_error("tensor_coordinate_out_of_range");
    if (dut_.request_valid || !dut_.request_ready) { ++stats_.queue_stalls; return {}; }
    drive(access,owner_id); dut_.request_index=dut_.request_slot; dut_.request_valid=1; dut_.eval();
    if (dut_.error) {
        const auto error=dut_.error; dut_.request_valid=0; dut_.eval();
        throw std::runtime_error(error==9 ? "tensor_column_not_allocated" : "tensor_memory_request_error");
    }
    const Ticket ticket=next_++; transfers_[dut_.request_slot]=Transfer{ticket,backing}; return ticket;
}
template<class D,uint32_t B> std::optional<std::array<uint32_t,32>> RtlTensorMemory<D,B>::take(Ticket ticket) {
    for (uint32_t slot=0; slot<8; ++slot) {
        if (!transfers_[slot] || transfers_[slot]->ticket!=ticket) continue;
        if (!(dut_.completed & (1u<<slot)) || (dut_.request_valid && dut_.request_slot==slot)) return {};
        dut_.query_slot=slot; dut_.eval(); std::array<uint32_t,32> result{};
        for (uint32_t lane=0; lane<32; ++lane) result[lane]=dut_.query_data[lane];
        dut_.acknowledge|=1u<<slot; dut_.eval(); transfers_[slot].reset(); return result;
    }
    throw std::runtime_error("invalid_tensor_memory_ticket");
}
template<class D,uint32_t B> size_t RtlTensorMemory<D,B>::outstanding() const {
    size_t count=0; for (const auto& transfer : transfers_) count+=bool(transfer); return count;
}

// Observe real SRAM write pins before the edge; snapshots never supply future read data.
// 在边沿前观察实际 SRAM 写引脚；快照绝不提供后续读取数据。
template<class D,uint32_t B> void RtlTensorMemory<D,B>::sample() {
    stats_.accepted+=bool(dut_.request_valid && dut_.request_ready); stats_.consumed+=std::popcount(uint32_t{dut_.acknowledge});
    if (dut_.command_accepted) {
        if (dut_.command_allocate) allocation_stats_.allocated+=dut_.command_count;
        else allocation_stats_.freed+=dut_.freed_columns;
    }
    allocation_stats_.reclaimed+=dut_.reclaimed_columns;
    for (auto lanes : dut_.blocked_lanes) stats_.bank_stalls+=std::popcount(uint32_t{lanes});
    for (uint32_t bank=0; bank<B; ++bank) {
        if (field(dut_.bank_accepted,bank,1)) {
            ++stats_.bank_services; stats_.broadcasts+=std::popcount(field(dut_.served_lanes,bank,32))-1;
        }
        if (!field(dut_.bank_writes,bank,1)) continue;
        auto* observation=observations_[field(dut_.write_owners,bank,6)];
        if (!observation) throw std::runtime_error("tensor_memory_write_without_owner");
        observation->write(field(dut_.write_words,bank,15)*4,4,field(dut_.write_values,bank,32));
    }
}
template<class D,uint32_t B> void RtlTensorMemory<D,B>::tick() {
    if (dut_.error) throw std::runtime_error("tensor_memory_protocol_error");
    ++stats_.clocks; sample();
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear(); dut_.eval(); allocation_stats_.peak_columns=std::max(allocation_stats_.peak_columns,uint64_t{dut_.used_columns});
}
}
std::unique_ptr<TensorMemory> make_rtl_tensor_memory(const TensorConfig& config) {
    switch (config.tmem_banks) {
    case 1: return std::make_unique<RtlTensorMemory<Vsimt_tmem1,1>>(config);
    case 2: return std::make_unique<RtlTensorMemory<Vsimt_tmem2,2>>(config);
    case 4: return std::make_unique<RtlTensorMemory<Vsimt_tmem4,4>>(config);
    case 8: return std::make_unique<RtlTensorMemory<Vsimt_tmem8,8>>(config);
    case 16: return std::make_unique<RtlTensorMemory<Vsimt_tmem16,16>>(config);
    case 32: return std::make_unique<RtlTensorMemory<Vsimt_tmem32,32>>(config);
    case 64: return std::make_unique<RtlTensorMemory<Vsimt_tmem64,64>>(config);
    case 128: return std::make_unique<RtlTensorMemory<Vsimt_tmem128,128>>(config);
    default: throw std::runtime_error("tensor_memory_rtl_requires_power_of_two_banks");
    }
}
}

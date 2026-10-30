#include "simt/cta_resources.hpp"
#include "Vsimt_cta_resources.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlCtaResources final : public CtaResources {
public:
    explicit RtlCtaResources(const PipelineConfig& config);
    ~RtlCtaResources() override { dut_.final(); }
    std::optional<CtaPlacement> acquire(uint32_t cta,uint32_t threads,uint32_t shared) override;
    void release(uint32_t cta) override { event(dut_.release_mask,cta); }
    void begin_release(uint32_t cta) override { event(dut_.begin_mask,cta); }
    void initialize_table(uint32_t cta) override { event(dut_.initialize_mask,cta); }
    void clean_table(uint32_t cta) override { event(dut_.clean_mask,cta); }
    void start_dma(uint32_t cta) override { event(dut_.dma_start_mask,cta); }
    void finish_dma(uint32_t cta) override { event(dut_.dma_finish_mask,cta); }
    void observe(const CtaResourceInputs& inputs) override;
    std::optional<CtaResourceQuery> query(uint32_t cta) override;
    CtaResourceState read() const override;
    bool pending() const override;
    void tick() override;
    const CtaResourceStats& stats() const override { return stats_; }
private:
    Vsimt_cta_resources dut_;
    CtaResourceStats stats_;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void clear();
    void event(QData& mask,uint32_t cta);
    void sample();
};
void RtlCtaResources::clear() {
    dut_.request_valid=0; dut_.release_mask=0; dut_.begin_mask=0;
    dut_.initialize_mask=0; dut_.clean_mask=0; dut_.dma_start_mask=0; dut_.dma_finish_mask=0;
}
RtlCtaResources::RtlCtaResources(const PipelineConfig& config) {
    const auto warps=std::min(config.max_warps,config.register_words/2048);
    if (!warps || warps>32 || !config.max_ctas || config.max_ctas>64 || !config.shared_bytes ||
        config.shared_bytes>65536 || !config.register_words || config.register_words>65536)
        throw std::runtime_error("invalid_cta_resource_rtl_configuration");
    clear(); dut_.cta_limit=config.max_ctas; dut_.warp_limit=warps;
    dut_.register_limit=config.register_words; dut_.shared_limit=config.shared_bytes;
    dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}

// A grant is copied directly from hardware and held until an explicit edge commits ownership.
// 直接复制硬件授权，并保持到显式边沿提交归属。
std::optional<CtaPlacement> RtlCtaResources::acquire(uint32_t cta,uint32_t threads,uint32_t shared) {
    if (dut_.request_valid) { ++stats_.admission_stalls; return {}; }
    dut_.request_cta=cta; dut_.request_threads=threads; dut_.request_shared=shared; dut_.eval();
    dut_.request_slot=dut_.grant_slot; dut_.request_warps=dut_.grant_warps;
    dut_.request_valid=1; dut_.eval();
    if (dut_.error || !dut_.accepted) {
        const auto error=dut_.error; dut_.request_valid=0; dut_.eval();
        if (error) throw std::runtime_error(error==2 || error==3 ? "invalid_cta_resource_request" : "cta_resource_allocation_error");
        ++stats_.admission_stalls; return {};
    }
    CtaPlacement result; result.slot=dut_.grant_slot;
    result.residency.registers=dut_.grant_registers; result.residency.shared=dut_.grant_shared;
    for (uint32_t warp=0; warp<32; ++warp) if (dut_.grant_warps & (1u<<warp)) result.residency.slots.push_back(warp);
    return result;
}
std::optional<CtaResourceQuery> RtlCtaResources::query(uint32_t cta) {
    dut_.query_cta=cta; dut_.eval();
    if (!dut_.query_found) return {};
    return CtaResourceQuery{dut_.query_slot,dut_.query_warps,dut_.query_shared,dut_.query_dma,
        bool(dut_.query_table_used),bool(dut_.query_table_clean),bool(dut_.query_releasing),bool(dut_.query_drained)};
}
void RtlCtaResources::event(QData& mask,uint32_t cta) {
    const auto owner=query(cta);
    if (!owner) throw std::runtime_error("unknown_resident_cta");
    const auto bit=uint64_t{1}<<owner->slot;
    if (mask & bit) throw std::runtime_error("duplicate_cta_resource_event");
    mask|=bit; dut_.eval();
    if (!dut_.error) return;
    mask&=~bit; dut_.eval(); throw std::runtime_error("cta_resource_event_error");
}

// Pack raw three-bit lane states; reduction and DMA/retirement decisions remain inside the circuit.
// 打包原始三位 lane 状态；归约和 DMA/退休决策仍在电路内部。
void RtlCtaResources::observe(const CtaResourceInputs& inputs) {
    for (const auto& warp : inputs.states) for (auto state : warp)
        if (state>5) throw std::runtime_error("invalid_cta_resource_lane_state");
    for (auto& word : dut_.lane_states) word=0;
    for (uint32_t warp=0; warp<32; ++warp) {
        dut_.inflight[warp]=inputs.inflight[warp];
        for (uint32_t lane=0; lane<32; ++lane) {
            const auto bit=(warp*32+lane)*3, word=bit/32, shift=bit%32;
            const auto value=uint64_t{inputs.states[warp][lane]}<<shift;
            dut_.lane_states[word]|=static_cast<uint32_t>(value);
            if (shift>29) dut_.lane_states[word+1]|=static_cast<uint32_t>(value>>32);
        }
    }
    dut_.eval();
}
CtaResourceState RtlCtaResources::read() const {
    CtaResourceState state{dut_.live,dut_.drained,dut_.cleanup,dut_.releasing,dut_.occupied_ctas,
        dut_.occupied_warps,dut_.occupied_registers,dut_.occupied_shared,{}};
    for (uint32_t slot=0; slot<64; ++slot) state.owners[slot]=dut_.owners[slot];
    return state;
}
bool RtlCtaResources::pending() const {
    return dut_.request_valid || dut_.release_mask || dut_.begin_mask || dut_.initialize_mask ||
        dut_.clean_mask || dut_.dma_start_mask || dut_.dma_finish_mask;
}
void RtlCtaResources::sample() {
    stats_.admitted+=bool(dut_.accepted); stats_.released+=std::popcount(uint64_t{dut_.release_mask});
    stats_.retiring+=std::popcount(uint64_t{dut_.begin_mask});
    stats_.table_initializations+=std::popcount(uint64_t{dut_.initialize_mask});
    stats_.table_cleanups+=std::popcount(uint64_t{dut_.clean_mask});
    stats_.dma_started+=std::popcount(uint64_t{dut_.dma_start_mask}); stats_.dma_finished+=std::popcount(uint64_t{dut_.dma_finish_mask});
}
void RtlCtaResources::tick() {
    if (dut_.error) throw std::runtime_error("cta_resource_protocol_error");
    ++stats_.clocks; sample();
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    clear(); dut_.eval();
    stats_.peak_ctas=std::max(stats_.peak_ctas,uint64_t{dut_.occupied_ctas});
    stats_.peak_registers=std::max(stats_.peak_registers,uint64_t{dut_.occupied_registers});
    stats_.peak_shared=std::max(stats_.peak_shared,uint64_t{dut_.occupied_shared});
}
}
std::unique_ptr<CtaResources> make_cta_resources(const PipelineConfig& config) { return std::make_unique<RtlCtaResources>(config); }
}

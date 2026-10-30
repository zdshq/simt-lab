#pragma once
#include "simt/hardware_pipeline.hpp"
#include <memory>

namespace simt::hw {
struct CtaPlacement { Residency residency; uint32_t slot=0; };
struct CtaResourceQuery {
    uint32_t slot=0, warps=0, shared=0, dma=0;
    bool table_used=false, table_clean=false, releasing=false, drained=false;
};
struct CtaResourceInputs {
    std::array<std::array<uint32_t,32>,32> states{};
    std::array<uint32_t,32> inflight{};
};
struct CtaResourceState {
    uint64_t live=0, drained=0, cleanup=0, releasing=0;
    uint32_t ctas=0, warps=0, registers=0, shared=0;
    std::array<uint32_t,64> owners{};
};
struct CtaResourceStats {
    uint64_t clocks=0, active_clocks=0, admitted=0, released=0, admission_stalls=0;
    uint64_t retiring=0, table_initializations=0, table_cleanups=0, dma_started=0, dma_finished=0;
    uint64_t peak_ctas=0, peak_registers=0, peak_shared=0;
};

// Physical ownership and retirement eligibility live in RTL; callers provide actual endpoint events.
// 物理归属与退休资格保存在 RTL；调用方提供实际端点事件。
class CtaResources {
public:
    virtual ~CtaResources()=default;
    virtual std::optional<CtaPlacement> acquire(uint32_t cta,uint32_t threads,uint32_t shared)=0;
    virtual void release(uint32_t cta)=0;
    virtual void begin_release(uint32_t cta)=0;
    virtual void initialize_table(uint32_t cta)=0;
    virtual void clean_table(uint32_t cta)=0;
    virtual void start_dma(uint32_t cta)=0;
    virtual void finish_dma(uint32_t cta)=0;
    virtual void observe(const CtaResourceInputs& inputs)=0;
    virtual std::optional<CtaResourceQuery> query(uint32_t cta)=0;
    virtual CtaResourceState read() const=0;
    virtual bool pending() const=0;
    virtual void tick()=0;
    virtual const CtaResourceStats& stats() const=0;
};
std::unique_ptr<CtaResources> make_cta_resources(const PipelineConfig& config);
}

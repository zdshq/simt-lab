#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace simt::hw {
enum class AsyncOwner : uint32_t { idle, instruction, dma, retirement };
struct AsyncServiceInputs {
    bool dma_valid=false, table_ready=false;
    uint64_t dma_id=0, retire_requests=0;
    uint32_t dma_cta=0;
    std::array<uint32_t,64> retire_ctas{};
};
struct AsyncServiceState {
    AsyncOwner kind=AsyncOwner::idle;
    uint64_t id=0;
    uint32_t cta=0, slot=0, cursor=0, remaining=0;
    bool ready=false, retirement_done=false, pending=false;
};
struct AsyncServiceStats {
    uint64_t clocks=0, active_clocks=0, retries=0, retirement_acks=0;
    std::array<uint64_t,3> accepted{}, completed{};
};

// Offered wires freeze after acceptance until the explicit edge; ownership and delay remain in RTL.
// 接受后提供的连线保持到显式时钟边沿；所有权和延迟均保存在 RTL 中。
class AsyncService {
public:
    virtual ~AsyncService()=default;
    virtual void offer(const AsyncServiceInputs& inputs)=0;
    virtual AsyncServiceState selection() const=0;
    virtual AsyncServiceState read() const=0;
    virtual bool foreground_ready() const=0;
    virtual void accept_instruction(uint64_t id,uint32_t cta)=0;
    virtual void accept_background()=0;
    virtual void complete()=0;
    virtual void retry()=0;
    virtual void acknowledge_retirement()=0;
    virtual void tick()=0;
    virtual const AsyncServiceStats& stats() const=0;
};
std::unique_ptr<AsyncService> make_async_service(uint32_t ctas,uint32_t latency);
}

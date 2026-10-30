#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace simt::hw {
struct ExecutionArbiterState {
    bool tensor_busy=false;
    uint32_t tensor_owner=0, tensor_cursor=0, async_cursor=0;
    std::array<uint32_t,24> cooldowns{};
};
struct ExecutionArbiterStats {
    uint64_t clocks=0, active_clocks=0, tensor_releases=0;
    std::array<uint64_t,7> requests{}, grants{}, accepted{}, interval_stalls{}, shared_stalls{}, arbitration_stalls{}, downstream_stalls{};
};

// RTL owns priorities, intervals and the Tensor lease; requests and completion events are raw inputs.
// RTL 保存优先级、间隔和 Tensor 归属；请求与完成事件是原始输入。
class ExecutionArbiter {
public:
    virtual ~ExecutionArbiter() = default;
    virtual void release_tensor(uint32_t partition) = 0;
    virtual uint64_t arbitrate(uint64_t requests,uint32_t async_noop,bool async_busy) = 0;
    virtual bool granted(uint32_t pipe,uint32_t partition) const = 0;
    virtual void accept(uint32_t pipe,uint32_t partition) = 0;
    virtual ExecutionArbiterState read() const = 0;
    virtual const ExecutionArbiterStats& stats() const = 0;
    virtual void tick() = 0;
};
std::unique_ptr<ExecutionArbiter> make_execution_arbiter(uint32_t partitions,std::array<uint32_t,3> intervals);
}

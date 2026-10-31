#pragma once
#include <cstdint>
#include <memory>
#include <optional>

namespace simt::hw {
enum class EngineKind : uint32_t { compute=0, copy=1, none=2 };
struct CpDispatch { bool node=false, cancel=false; uint32_t slot=0; };
struct CpNode { uint32_t handle=0; EngineKind kind=EngineKind::none; bool cancel=false; };
struct CpStats { uint64_t clocks=0, active_clocks=0, fetched=0, dispatched=0, cancelled=0, completed=0, failed=0, peak_slots=0; };

// Pins hold until tick(); reads never advance the circuit or the device clock.
// 引脚保持到 tick()；读取不会推进电路或设备时钟。
class CommandProcessor {
public:
    virtual ~CommandProcessor()=default;
    virtual void ring_doorbell(uint64_t put)=0;
    virtual uint64_t published() const=0;
    virtual uint64_t fetched() const=0;
    virtual uint64_t notified() const=0;
    virtual std::optional<uint32_t> fetch_slot() const=0;
    virtual uint32_t lookup(uint32_t handle)=0;
    virtual void accept_fetch(uint32_t handle,EngineKind kind,uint32_t wait)=0;
    virtual void offer_external(uint32_t pending,uint32_t failed)=0;
    virtual void offer_node(const std::optional<CpNode>& node)=0;
    virtual std::optional<CpDispatch> dispatch() const=0;
    virtual std::optional<uint32_t> select_graph(uint32_t pending)=0;
    virtual void complete(uint32_t slot,bool failed)=0;
    virtual void release(EngineKind kind)=0;
    virtual bool busy(EngineKind kind) const=0;
    virtual void tick()=0;
    virtual const CpStats& stats() const=0;
};
std::unique_ptr<CommandProcessor> make_command_processor(uint32_t capacity);
}

#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace simt::hw {
struct ControlLanes {
    std::array<uint32_t,32> pc{}, state{}, blocked{};
};
struct ControlCommand {
    uint32_t opcode = 0, token = 0, guard = 7, pc = 0, group = 0, mask = 0, target = 0, code_size = 0;
    bool target_is_sync = false;
};
struct ControlToken { uint32_t state = 0, members = 0, arrivals = 0, target = 0; };
struct ControlResult { ControlLanes lanes; std::array<ControlToken,16> tokens; };
struct ControlStats { uint64_t clocks = 0, active_clocks = 0, commands = 0, clears = 0, woken_lanes = 0; };

// The engine owns token storage; caller lane vectors are explicit ingress and snapshots are observations.
// 引擎持有令牌存储；调用方 lane 向量是显式输入，快照仅供观察。
class WarpControl {
public:
    virtual ~WarpControl() = default;
    virtual void clear() = 0;
    virtual ControlResult execute(const ControlCommand& command, const ControlLanes& lanes) = 0;
    virtual void tick() = 0;
    virtual const ControlStats& stats() const = 0;
};
std::unique_ptr<WarpControl> make_warp_control();
}

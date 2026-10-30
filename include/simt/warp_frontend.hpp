#pragma once
#include "simt/warp_control.hpp"
#include <optional>

namespace simt::hw {
struct FrontendState {
    ControlResult control;
    std::array<uint32_t,32> phase{};
    uint32_t valid_mask = 0;
    std::optional<uint32_t> last_pc;
};
struct FrontendGroup { uint32_t pc = 0, mask = 0; };
struct FrontendWait { std::array<uint32_t,32> state{}, blocked{}, phase{}; };
struct FrontendStats {
    uint64_t clocks = 0, active_clocks = 0, issued = 0, advances = 0, external_lanes = 0, resumed_lanes = 0;
    uint64_t cta_blocked = 0, cta_woken = 0;
};

// Hardware owns PC, lane states, wait metadata, the PC-group cursor and all reconvergence tokens.
// 硬件持有 PC、lane 状态、等待元数据、PC 组游标及全部重聚令牌。
class WarpFrontend {
public:
    virtual ~WarpFrontend() = default;
    virtual void initialize(uint32_t valid_mask) = 0;
    virtual FrontendState read() const = 0;
    virtual FrontendState execute(const ControlCommand& command) = 0;
    virtual std::optional<FrontendGroup> select(uint32_t policy) = 0;
    virtual void issue(FrontendGroup group, bool advance) = 0;
    virtual void advance(FrontendGroup group) = 0;
    virtual void external(const FrontendWait& waits) = 0;
    virtual void cta_barrier(uint32_t block_mask, uint32_t barrier, uint32_t release_mask) = 0;
    virtual void tick() = 0;
    virtual const ControlStats& stats() const = 0;
    virtual const FrontendStats& frontend_stats() const = 0;
};
std::unique_ptr<WarpFrontend> make_warp_frontend();
}

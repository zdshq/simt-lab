#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace simt::hw {
struct CtaBarrierCommand { bool exit = false; uint32_t warp = 0, mask = 0, barrier = 0, count = 0; };
struct CtaBarrierRecord {
    bool active = false;
    uint32_t count = 0, arrived = 0, expected = 0;
    uint64_t generation = 0;
    std::array<uint32_t,32> members{};
};
struct CtaBarrierState {
    uint32_t threads = 0, live_count = 0;
    std::array<uint32_t,32> live{};
    std::array<CtaBarrierRecord,16> records;
};
struct CtaBarrierStats {
    uint64_t clocks = 0, active_clocks = 0, initializations = 0, commands = 0;
    uint64_t arrivals = 0, exits = 0, arrival_lanes = 0, exit_lanes = 0, completions = 0;
};

// Hardware owns live threads and every barrier generation; the returned release bus is not computed by the host.
// 硬件持有存活线程和每个屏障阶段，返回的释放总线不由宿主计算。
class CtaBarrierEngine {
public:
    virtual ~CtaBarrierEngine() = default;
    virtual void initialize(uint32_t threads) = 0;
    virtual bool available() const = 0;
    virtual uint32_t execute(const CtaBarrierCommand& command) = 0;
    virtual CtaBarrierState read() const = 0;
    virtual void tick() = 0;
    virtual const CtaBarrierStats& stats() const = 0;
};
std::unique_ptr<CtaBarrierEngine> make_cta_barrier();
}

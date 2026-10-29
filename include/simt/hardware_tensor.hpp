#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>

namespace simt::hw {
struct TensorConfig {
    uint32_t rows = 4, columns = 4, latency = 4, input_capacity = 2;
    uint32_t tmem_banks = 16, tmem_latency = 2, tmem_columns = 256;
    bool rtl = false;
};
void validate_tensor_config(const TensorConfig& config);
struct TensorWave {
    std::array<uint32_t, 4> a{}, b{}, scale_a{}, scale_b{};
    std::array<bool, 4> enabled{true, true, true, true};
    bool block_end = false, last = false;
};
struct ArrayStats {
    uint64_t waves = 0, products = 0, skipped = 0, scale_products = 0, scale_accumulates = 0;
    uint64_t input_stalls = 0, queue_stalls = 0, tiles = 0;
};
struct ArrayToken {
    uint64_t sequence = 0;
    uint32_t value = 0, scale = 0;
    bool enabled = true, block_end = false, last = false;
};
enum class MacStage { dot, scale, accumulate };
struct MacPending {
    uint64_t due;
    uint32_t result, scale_a, scale_b;
    bool block_end, last;
    MacStage stage = MacStage::dot;
};
struct MacCell {
    uint32_t accumulator = 0, partial = 0;
    std::optional<MacPending> pending;
};
struct InjectedWave { TensorWave wave; uint64_t cycle, sequence; };

// A/B tokens move one neighboring PE per clock; each PE owns its accumulator and pending result.
// A/B 令牌每拍移动到相邻 PE，每个 PE 独占累加器和待完成结果。
class TensorArray {
public:
    explicit TensorArray(TensorConfig config = {});
    void begin(uint32_t rows, uint32_t columns, bool integer, bool scaled,
               const std::array<uint32_t, 16>& initial = {});
    bool push(const TensorWave& wave);
    void tick();
    std::optional<std::array<uint32_t, 16>> take();
    const ArrayStats& stats() const { return stats_; }
    uint64_t cycle() const { return cycle_; }
    bool busy() const { return active_; }
private:
    TensorConfig config_;
    ArrayStats stats_;
    std::array<MacCell, 16> cells_{};
    std::array<std::optional<ArrayToken>, 16> a_{}, b_{};
    std::deque<TensorWave> inputs_;
    std::deque<InjectedWave> injected_;
    uint64_t cycle_ = 0, available_ = 0, sequence_ = 0;
    uint32_t rows_ = 0, columns_ = 0, finished_ = 0;
    bool active_ = false, integer_ = false, scaled_ = false, last_ = false;
    void inject();
    void move();
    std::optional<ArrayToken> edge(uint32_t offset, bool left) const;
    void multiply(uint32_t row, uint32_t column, const ArrayToken& a, const ArrayToken& b);
    void retire(MacCell& cell);
};
}

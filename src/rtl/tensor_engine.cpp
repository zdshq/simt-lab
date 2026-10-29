#include "simt/tensor_engine.hpp"
#include "Vsimt_tensor.h"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlTensorEngine final : public TensorEngine {
public:
    explicit RtlTensorEngine(const TensorConfig& config) : config_(config) {
        if (config.latency != 4 || config.input_capacity != 2)
            throw std::runtime_error("tensor_rtl_requires_latency_4_and_queue_2");
        dut_.clk = 0; dut_.reset = 1;
        dut_.start_valid = 0; dut_.wave_valid = 0; dut_.result_ready = 0; dut_.eval();
    }
    ~RtlTensorEngine() override { dut_.final(); }
    void begin(uint32_t rows, uint32_t columns, uint32_t format, bool scaled,
               const std::array<uint32_t, 16>& initial) override;
    bool push(const TensorInput& input) override;
    void tick() override;
    std::optional<std::array<uint32_t, 16>> take() override;
    const ArrayStats& stats() const override { return stats_; }
    uint64_t cycle() const override { return cycle_; }
    bool busy() const override { return active_; }
    std::string_view name() const override { return "rtl"; }
private:
    Vsimt_tensor dut_;
    TensorConfig config_;
    ArrayStats stats_;
    uint64_t cycle_ = 0;
    bool active_ = false, last_ = false, scaled_ = false;
    void drive(const TensorInput& input);
    void observe(bool start);
    void check_protocol() const;
};

// Commands set stable pins only; all sequential state changes are confined to tick().
// 命令只设置稳定引脚，所有时序状态变化都限制在 tick() 中。
void RtlTensorEngine::begin(uint32_t rows, uint32_t columns, uint32_t format, bool scaled,
                            const std::array<uint32_t, 16>& initial) {
    if (active_ || dut_.start_valid) throw std::runtime_error("tensor_array_busy");
    if (!rows || rows > config_.rows || !columns || columns > config_.columns || format > 4 || (format == 3 && scaled))
        throw std::runtime_error("invalid_tensor_array_shape");
    dut_.start_rows = rows; dut_.start_columns = columns; dut_.start_format = format; dut_.start_scaled = scaled;
    for (uint32_t cell = 0; cell < 16; ++cell) dut_.initial_values[cell] = initial[cell];
    dut_.start_valid = 1; active_ = true; last_ = false; scaled_ = scaled; dut_.eval();
}

void RtlTensorEngine::drive(const TensorInput& input) {
    dut_.wave_enabled = 0; dut_.wave_high_a = 0; dut_.wave_high_b = 0;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        dut_.wave_a[lane] = input.raw_a[lane]; dut_.wave_b[lane] = input.raw_b[lane];
        dut_.wave_scale_a[lane] = input.scale_a[lane]; dut_.wave_scale_b[lane] = input.scale_b[lane];
        dut_.wave_enabled |= uint32_t(input.enabled[lane]) << lane;
        dut_.wave_high_a |= uint32_t(input.high_a[lane]) << lane;
        dut_.wave_high_b |= uint32_t(input.high_b[lane]) << lane;
    }
    dut_.wave_block_end = input.block_end; dut_.wave_last = input.last;
}

// A successful call reserves the one physical input port until the next device clock.
// 调用成功后保留唯一物理输入端口，直到下一个设备时钟。
bool RtlTensorEngine::push(const TensorInput& input) {
    if (!active_ || last_) throw std::runtime_error("tensor_array_invalid_feed");
    if (scaled_ && input.last && !input.block_end) throw std::runtime_error("tensor_array_missing_block_end");
    if (dut_.start_valid || dut_.wave_valid) { ++stats_.queue_stalls; return false; }
    drive(input); dut_.eval();
    if (!dut_.wave_ready) { ++stats_.queue_stalls; return false; }
    dut_.wave_valid = 1; last_ = input.last; dut_.eval();
    return true;
}

// Only real RTL injection/PE events contribute work counts; no host-side matrix arithmetic is performed.
// 仅真实 RTL 注入和 PE 事件计入工作量，不在宿主执行矩阵算术。
void RtlTensorEngine::observe(bool start) {
    stats_.tiles += start; stats_.waves += dut_.event_wave;
    stats_.products += std::popcount(uint32_t(dut_.event_products));
    stats_.skipped += std::popcount(uint32_t(dut_.event_skips));
    stats_.scale_products += std::popcount(uint32_t(dut_.event_scales));
    stats_.scale_accumulates += std::popcount(uint32_t(dut_.event_accumulates));
    stats_.input_stalls += dut_.event_input_stall;
}

void RtlTensorEngine::check_protocol() const {
    if (dut_.command_error) throw std::runtime_error("tensor_rtl_command_error");
    if (dut_.transport_error) throw std::runtime_error("tensor_rtl_transport_error");
}

void RtlTensorEngine::tick() {
    dut_.clk = 0; dut_.eval();
    const bool reset = dut_.reset, start = dut_.start_valid && dut_.start_ready;
    const bool feed = dut_.wave_valid && dut_.wave_ready, take = dut_.result_valid && dut_.result_ready;
    if (!reset) { check_protocol(); observe(start); }
    dut_.clk = 1; dut_.eval(); ++cycle_;
    dut_.clk = 0; dut_.eval();
    if (reset) dut_.reset = 0;
    if (start) dut_.start_valid = 0;
    if (feed) dut_.wave_valid = 0;
    if (take) dut_.result_ready = 0;
    dut_.eval(); check_protocol();
}

std::optional<std::array<uint32_t, 16>> RtlTensorEngine::take() {
    dut_.eval();
    if (!active_ || dut_.start_valid || dut_.result_ready || !dut_.result_valid) return {};
    std::array<uint32_t, 16> result{};
    for (uint32_t cell = 0; cell < 16; ++cell) result[cell] = dut_.result_values[cell];
    dut_.result_ready = 1; active_ = false; dut_.eval();
    return result;
}
}

std::unique_ptr<TensorEngine> make_rtl_tensor_engine(const TensorConfig& config) {
    return std::make_unique<RtlTensorEngine>(config);
}
}

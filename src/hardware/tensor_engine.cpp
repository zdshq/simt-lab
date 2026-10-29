#include "simt/tensor_engine.hpp"
#include "simt/tensor.hpp"
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
uint32_t decode(uint32_t bits, uint32_t format, bool high) {
    if (format == 3) return uint32_t(std::bit_cast<int8_t>(uint8_t(bits)));
    return float_bits(decode_tensor_float(bits, high, format));
}

TensorWave decode_input(const TensorInput& input, uint32_t format) {
    TensorWave wave;
    wave.enabled = input.enabled; wave.block_end = input.block_end; wave.last = input.last;
    wave.scale_a = input.scale_a; wave.scale_b = input.scale_b;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        wave.a[lane] = input.enabled[lane] ? decode(input.raw_a[lane], format, input.high_a[lane]) : 0;
        wave.b[lane] = decode(input.raw_b[lane], format, input.high_b[lane]);
    }
    return wave;
}

// The reference model remains independently usable and is never a fallback for a requested RTL backend.
// 参考模型仍可独立使用，但不会代替用户明确请求的 RTL 后端。
class ModelTensorEngine final : public TensorEngine {
public:
    explicit ModelTensorEngine(const TensorConfig& config) : array_(config) {}
    void begin(uint32_t rows, uint32_t columns, uint32_t format, bool scaled,
               const std::array<uint32_t, 16>& initial) override {
        if (format > 4) throw std::runtime_error("invalid_tensor_format");
        array_.begin(rows, columns, format == 3, scaled, initial); format_ = format;
    }
    bool push(const TensorInput& input) override { return array_.push(decode_input(input, format_)); }
    void tick() override { array_.tick(); }
    std::optional<std::array<uint32_t, 16>> take() override { return array_.take(); }
    const ArrayStats& stats() const override { return array_.stats(); }
    uint64_t cycle() const override { return array_.cycle(); }
    bool busy() const override { return array_.busy(); }
    std::string_view name() const override { return "model"; }
private:
    TensorArray array_;
    uint32_t format_ = 0;
};
}

bool tensor_rtl_available() {
#ifdef SIMT_HAS_TENSOR_RTL
    return true;
#else
    return false;
#endif
}

std::unique_ptr<TensorEngine> make_tensor_engine(const TensorConfig& config) {
    validate_tensor_config(config);
    if (!config.rtl) return std::make_unique<ModelTensorEngine>(config);
#ifdef SIMT_HAS_TENSOR_RTL
    return make_rtl_tensor_engine(config);
#else
    throw std::runtime_error("tensor_rtl_backend_unavailable; build with SIMT_TENSOR_RTL=ON");
#endif
}
}

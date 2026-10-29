#pragma once
#include "simt/hardware_tensor.hpp"
#include <memory>
#include <string_view>

namespace simt::hw {
// The frontend supplies fetched bytes; conversion belongs to the selected arithmetic backend.
// 前端提供已取回的字节，格式转换由所选算术后端完成。
struct TensorInput {
    std::array<uint32_t, 4> raw_a{}, raw_b{}, scale_a{}, scale_b{};
    std::array<bool, 4> enabled{true, true, true, true}, high_a{}, high_b{};
    bool block_end = false, last = false;
};

class TensorEngine {
public:
    virtual ~TensorEngine() = default;
    virtual void begin(uint32_t rows, uint32_t columns, uint32_t format, bool scaled,
                       const std::array<uint32_t, 16>& initial) = 0;
    virtual bool push(const TensorInput& input) = 0;
    virtual void tick() = 0;
    virtual std::optional<std::array<uint32_t, 16>> take() = 0;
    virtual const ArrayStats& stats() const = 0;
    virtual uint64_t cycle() const = 0;
    virtual bool busy() const = 0;
    virtual std::string_view name() const = 0;
};

bool tensor_rtl_available();
std::unique_ptr<TensorEngine> make_tensor_engine(const TensorConfig& config);
std::unique_ptr<TensorEngine> make_rtl_tensor_engine(const TensorConfig& config);
}

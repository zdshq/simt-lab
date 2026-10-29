#include "simt/hardware_tensor.hpp"
#include "simt/execution.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace simt::hw {
TensorArray::TensorArray(TensorConfig config) : config_(config) {
    if (!config.rows || config.rows > 4 || !config.columns || config.columns > 4 || !config.latency ||
        !config.input_capacity || config.input_capacity > 32 || !config.tmem_banks || config.tmem_banks > 128 ||
        !config.tmem_latency || !config.tmem_columns || config.tmem_columns > 256 || config.tmem_columns % 8)
        throw std::runtime_error("invalid_tensor_pipeline_config");
}

// An unconsumed tile still owns the array, bounding both request and response storage.
// 尚未取走的 tile 仍占有阵列，以限制请求和响应存储容量。
void TensorArray::begin(uint32_t rows, uint32_t columns, bool integer, bool scaled,
                        const std::array<uint32_t, 16>& initial) {
    if (active_) throw std::runtime_error("tensor_array_busy");
    if (!rows || rows > config_.rows || !columns || columns > config_.columns || (integer && scaled))
        throw std::runtime_error("invalid_tensor_array_shape");
    rows_ = rows; columns_ = columns; integer_ = integer; scaled_ = scaled;
    active_ = true; last_ = false; finished_ = 0; available_ = cycle_;
    a_ = {}; b_ = {}; cells_ = {}; inputs_.clear(); injected_.clear();
    for (uint32_t index = 0; index < cells_.size(); ++index) cells_[index].accumulator = initial[index];
    ++stats_.tiles;
}

bool TensorArray::push(const TensorWave& wave) {
    if (!active_ || last_) throw std::runtime_error("tensor_array_invalid_feed");
    if (inputs_.size() == config_.input_capacity) { ++stats_.queue_stalls; return false; }
    if (scaled_ && wave.last && !wave.block_end) throw std::runtime_error("tensor_array_missing_block_end");
    inputs_.push_back(wave); last_ = wave.last;
    return true;
}

// Block scaling reuses each PE for scale multiplication and final fused accumulation.
// 块缩放复用各 PE，依次计算缩放乘积和最终融合累加。
void TensorArray::inject() {
    if (available_ > cycle_) return;
    if (inputs_.empty()) { if (!last_) ++stats_.input_stalls; return; }
    const auto wave = inputs_.front(); inputs_.pop_front();
    injected_.push_back({wave, cycle_, sequence_++}); ++stats_.waves;
    available_ = cycle_ + uint64_t{config_.latency} * (scaled_ && wave.block_end ? 3 : 1);
}

std::optional<ArrayToken> TensorArray::edge(uint32_t offset, bool left) const {
    for (const auto& injected : injected_) {
        if (injected.cycle + offset != cycle_) continue;
        const auto& wave = injected.wave;
        return ArrayToken{injected.sequence, left ? wave.a[offset] : wave.b[offset],
                          left ? wave.scale_a[offset] : wave.scale_b[offset],
                          left ? wave.enabled[offset] : true, wave.block_end, wave.last};
    }
    return {};
}

// Skew the two edges so corresponding K tokens meet at PE(row,column) on the same clock.
// 在两侧错拍注入，使同一 K 的令牌在同拍到达 PE(row,column)。
void TensorArray::move() {
    std::array<std::optional<ArrayToken>, 16> next_a{}, next_b{};
    for (uint32_t row = 0; row < rows_; ++row) for (uint32_t column = 0; column < columns_; ++column) {
        const auto index = row * 4 + column;
        next_a[index] = column ? a_[index - 1] : edge(row, true);
        next_b[index] = row ? b_[index - 4] : edge(column, false);
        if (bool(next_a[index]) != bool(next_b[index])) throw std::runtime_error("tensor_wave_alignment");
        if (next_a[index]) multiply(row, column, *next_a[index], *next_b[index]);
    }
    a_ = std::move(next_a); b_ = std::move(next_b);
    while (!injected_.empty() && injected_.front().cycle + std::max(rows_, columns_) <= cycle_)
        injected_.pop_front();
}

// Sparse absent entries consume transport clocks but perform no arithmetic, including on NaN B.
// 稀疏缺失项占用传输时钟，但不执行算术，即使对应 B 为 NaN。
void TensorArray::multiply(uint32_t row, uint32_t column, const ArrayToken& a, const ArrayToken& b) {
    auto& cell = cells_[row * 4 + column];
    if (a.sequence != b.sequence || cell.pending) throw std::runtime_error("tensor_pe_dependency_violation");
    auto value = scaled_ ? cell.partial : cell.accumulator;
    if (!a.enabled) ++stats_.skipped;
    else {
        ++stats_.products;
        if (integer_) value += static_cast<uint32_t>(int64_t{std::bit_cast<int32_t>(a.value)} * std::bit_cast<int32_t>(b.value));
        else value = float_bits(std::fma(to_float(a.value), to_float(b.value), to_float(value)));
    }
    cell.pending = MacPending{cycle_ + config_.latency, value, a.scale, b.scale, a.block_end, a.last};
}

void TensorArray::retire(MacCell& cell) {
    if (!cell.pending || cell.pending->due > cycle_) return;
    auto& pending = *cell.pending;
    if (scaled_ && pending.stage == MacStage::dot) {
        cell.partial = pending.result;
        if (pending.block_end) {
            pending.result = float_bits(to_float(pending.scale_a) * to_float(pending.scale_b));
            pending.stage = MacStage::scale; pending.due += config_.latency; ++stats_.scale_products; return;
        }
    } else if (pending.stage == MacStage::scale) {
        pending.result = float_bits(std::fma(to_float(cell.partial), to_float(pending.result), to_float(cell.accumulator)));
        pending.stage = MacStage::accumulate; pending.due += config_.latency; ++stats_.scale_accumulates; return;
    } else { cell.accumulator = pending.result; cell.partial = 0; }
    if (pending.last) ++finished_;
    cell.pending.reset();
}

void TensorArray::tick() {
    ++cycle_;
    if (!active_) return;
    for (auto& cell : cells_) retire(cell);
    inject(); move();
}

std::optional<std::array<uint32_t, 16>> TensorArray::take() {
    if (!active_ || finished_ != rows_ * columns_) return {};
    std::array<uint32_t, 16> result{};
    for (uint32_t index = 0; index < cells_.size(); ++index) result[index] = cells_[index].accumulator;
    active_ = false;
    return result;
}
}

#include "simt/timing.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace simt {
namespace {
void element_read(TensorFlight& flight, uint32_t base, uint64_t index, uint32_t slot) {
    const auto& d = flight.descriptor;
    const auto width = tensor_element_size(d.format);
    const auto address = uint64_t{base} + (d.format == 4 ? index / 2 : index * width);
    tensor_transfer(flight, d.space ? 0 : 1, address, width, slot);
}

uint32_t checked_scale(uint32_t bits) {
    const auto value = to_float(bits);
    if (!std::isfinite(value) || value < 0) throw std::runtime_error("invalid_block_scale");
    return bits;
}
}

// Sparse metadata is fetched through the same input memory path before compact A addresses exist.
// 紧凑 A 地址生成前，稀疏元数据先经过相同输入内存路径读取。
void TimingMachine::prepare_tensor_wave(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    flight.wave = {}; flight.fetched = {};
    flight.wave.last = flight.k + 1 == d.k;
    flight.wave.block_end = (d.flags & 4) && (flight.k + 1) % d.block_k == 0;
    if (!(d.flags & 2)) { prepare_tensor_operands(entry); return; }
    for (uint32_t row = 0; row < flight.rows; ++row)
        tensor_transfer(flight, d.space ? 0 : 1, uint64_t{d.metadata} + uint64_t{flight.row + row} * d.stride_metadata + flight.k / 4, 1, row);
    flight.phase = TensorPhase::metadata;
}

void TimingMachine::tensor_metadata(Inflight& entry) {
    auto& flight = *entry.tensor;
    if (!poll_tensor_transfers(entry)) return;
    for (uint32_t row = 0; row < flight.rows; ++row) {
        const auto metadata = flight.fetched[row], first = metadata & 3, second = (metadata >> 2) & 3;
        if ((metadata & 0xF0) || first >= second) throw std::runtime_error("invalid_sparse_metadata");
        flight.wave.enabled[row] = flight.k % 4 == first || flight.k % 4 == second;
        flight.indices_a[row] = flight.k / 4 * 2 + (flight.k % 4 == second);
    }
    prepare_tensor_operands(entry);
}

// The bounded staging buffer reads one outer-product wave while the array consumes older waves.
// 有限暂存缓冲区读取一组外积操作数，阵列同时消费较早的操作数组。
void TimingMachine::prepare_tensor_operands(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    flight.fetched = {}; bool any = false;
    for (uint32_t row = 0; row < flight.rows; ++row) {
        if (!flight.wave.enabled[row]) continue;
        const auto k = d.flags & 2 ? flight.indices_a[row] : flight.k;
        flight.indices_a[row] = uint64_t{flight.row + row} * d.stride_a + k;
        element_read(flight, d.a, flight.indices_a[row], row); any = true;
    }
    if (any) for (uint32_t column = 0; column < flight.columns; ++column)
        element_read(flight, d.b, uint64_t{flight.k} * d.stride_b + flight.column + column, 4 + column);
    if (flight.wave.block_end) prepare_tensor_scales(entry);
    flight.phase = TensorPhase::operands;
}

void TimingMachine::prepare_tensor_scales(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    const auto block = flight.k / d.block_k;
    for (uint32_t row = 0; row < flight.rows; ++row)
        tensor_transfer(flight, d.space ? 0 : 1, uint64_t{d.scale_a} + (uint64_t{flight.row + row} * d.stride_scale_a + block) * 4, 4, 8 + row);
    for (uint32_t column = 0; column < flight.columns; ++column)
        tensor_transfer(flight, d.space ? 0 : 1, uint64_t{d.scale_b} + (uint64_t{block} * d.stride_scale_b + flight.column + column) * 4, 4, 12 + column);
}

void TimingMachine::decode_tensor_operands(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    if (!poll_tensor_transfers(entry)) return;
    for (uint32_t row = 0; row < flight.rows; ++row) {
        if (flight.wave.enabled[row]) flight.wave.raw_a[row] = flight.fetched[row];
        flight.wave.high_a[row] = flight.indices_a[row] % 2;
        if (flight.wave.block_end) flight.wave.scale_a[row] = checked_scale(flight.fetched[8 + row]);
    }
    for (uint32_t column = 0; column < flight.columns; ++column) {
        flight.wave.raw_b[column] = flight.fetched[4 + column];
        flight.wave.high_b[column] = (uint64_t{flight.k} * d.stride_b + flight.column + column) % 2;
        if (flight.wave.block_end) flight.wave.scale_b[column] = checked_scale(flight.fetched[12 + column]);
    }
    flight.phase = TensorPhase::feed;
}
}

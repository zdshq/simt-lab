#include "simt/timing.hpp"
#include <algorithm>

namespace simt {
void TimingMachine::tensor_descriptor(Inflight& entry) {
    auto& flight = *entry.tensor;
    if (!poll_tensor_transfers(entry)) return;
    std::array<uint32_t, 20> words{};
    std::copy_n(flight.fetched.begin(), 20, words.begin());
    flight.descriptor = decode_descriptor(words);
    const auto& d = flight.descriptor;
    for (uint32_t row = 0; row < d.m; ++row) for (uint32_t column = 0; column < d.n; ++column)
        tensor_address(machine_.ctas_[entry.cta], d.row_d + row, d.column_d + column);
    trace(entry, "tensor_descriptor"); begin_tensor_tile(entry);
}

// Every output microtile explicitly loads D through TMEM banks when accumulation is requested.
// 请求累加时，每个输出微 tile 都显式经过 TMEM banks 装载 D。
void TimingMachine::begin_tensor_tile(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    flight.rows = std::min(machine_.config_.tensor.rows, d.m - flight.row);
    flight.columns = std::min(machine_.config_.tensor.columns, d.n - flight.column);
    flight.k = 0; flight.fetched = {};
    if (d.flags & 1) for (uint32_t row = 0; row < flight.rows; ++row) for (uint32_t column = 0; column < flight.columns; ++column) {
        const auto address = tensor_address(machine_.ctas_[entry.cta], d.row_d + flight.row + row, d.column_d + flight.column + column);
        tensor_transfer(flight, 2, address, 4, row * 4 + column);
    }
    flight.phase = TensorPhase::initial;
}

void TimingMachine::initial_tensor_tile(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    if (!poll_tensor_transfers(entry)) return;
    std::array<uint32_t, 16> initial{};
    std::copy_n(flight.fetched.begin(), 16, initial.begin());
    sms_[entry.sm]->array.begin(flight.rows, flight.columns, d.format == 3, d.flags & 4, initial);
    trace(entry, "tensor_tile_start"); prepare_tensor_wave(entry);
}

void TimingMachine::feed_tensor(Inflight& entry) {
    auto& flight = *entry.tensor;
    if (!sms_[entry.sm]->array.push(flight.wave)) return;
    trace(entry, "tensor_feed"); ++flight.k;
    if (flight.k == flight.descriptor.k) flight.phase = TensorPhase::drain;
    else prepare_tensor_wave(entry);
}

// Results remain inside the array until every PE has completed its last pipeline stage.
// 所有 PE 完成最后一级流水线之前，结果保持在阵列内部。
void TimingMachine::drain_tensor(Inflight& entry) {
    auto& flight = *entry.tensor; const auto& d = flight.descriptor;
    const auto result = sms_[entry.sm]->array.take();
    if (!result) return;
    for (uint32_t row = 0; row < flight.rows; ++row) for (uint32_t column = 0; column < flight.columns; ++column) {
        const auto address = tensor_address(machine_.ctas_[entry.cta], d.row_d + flight.row + row, d.column_d + flight.column + column);
        tensor_transfer(flight, 2, address, 4, row * 4 + column, true, (*result)[row * 4 + column]);
    }
    trace(entry, "tensor_array_complete"); flight.phase = TensorPhase::store;
}

void TimingMachine::store_tensor(Inflight& entry) {
    auto& flight = *entry.tensor;
    if (!poll_tensor_transfers(entry)) return;
    trace(entry, "tensor_tile_store");
    flight.column += flight.columns;
    if (flight.column == flight.descriptor.n) { flight.column = 0; flight.row += flight.rows; }
    if (flight.row == flight.descriptor.m) flight.phase = TensorPhase::complete;
    else begin_tensor_tile(entry);
}
}

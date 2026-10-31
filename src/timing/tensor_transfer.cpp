#include "simt/timing.hpp"
#include <bit>

namespace simt {
// Group equally sized accesses into finite vector requests while preserving return destinations.
// 将同宽访问合并为有限向量请求，同时保留返回数据的目标位置。
void tensor_transfer(TensorFlight& flight, uint32_t space, uint64_t address, uint32_t width,
                     uint32_t slot, bool write, uint32_t value) {
    TensorTransfer* selected = nullptr;
    for (auto& part : flight.transfers)
        if (part.space == space && part.access.width == width && part.access.write == write && part.access.mask != UINT32_MAX)
            selected = &part;
    if (!selected) {
        hw::WarpAccess access; access.mask = 0; access.width = width; access.write = write;
        flight.transfers.push_back({space, access, {}, {}, false}); selected = &flight.transfers.back();
    }
    const auto lane = static_cast<uint32_t>(std::popcount(selected->access.mask));
    selected->access.mask |= lane_bit(lane); selected->access.address[lane] = address;
    selected->access.value[lane] = value; selected->slots[lane] = slot;
}

// All bytes, including descriptors, scales, and accumulators, return through modeled ports.
// 所有字节，包括描述符、缩放系数和累加器，都通过模型端口返回。
bool TimingMachine::poll_tensor_transfers(Inflight& entry) {
    auto& flight = *entry.tensor; auto& cta = machine_.ctas_[entry.cta];
    for (const auto& part : flight.transfers)
        hw::validate_access(part.access, part.space == 0 ? *machine_.global_ : part.space == 1 ? cta.shared : cta.tmem);
    bool done = true;
    for (auto& part : flight.transfers) done &= service_tensor_transfer(entry, part);
    if (done) flight.transfers.clear();
    return done;
}

// Retry only unaccepted packets; a consumed response can never be replayed.
// 仅重试尚未接受的包，已消费响应不会被重放。
bool TimingMachine::service_tensor_transfer(Inflight& entry, TensorTransfer& part) {
    if (part.done) return true;
    auto& sm = *sms_[entry.sm]; auto& cta = machine_.ctas_[entry.cta];
    auto& port = part.space == 1 ? *sm.shared : static_cast<hw::SharedPort&>(*sm.tmem);
    auto& backing = part.space == 1 ? cta.shared : cta.tmem;
    if (!part.ticket) {
        part.ticket = part.space == 0 ? submit_memory(entry.sm, part.access) : port.submit(part.access, &backing);
        if (part.ticket) trace(entry, "tensor_memory_submit");
    }
    const auto result = !part.ticket ? std::optional<std::array<uint32_t, 32>>{} :
        part.space == 0 ? memory_->take(*part.ticket) : port.take(*part.ticket);
    if (!result) return false;
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if ((part.access.mask & lane_bit(lane)) && !part.access.write) entry.tensor->fetched[part.slots[lane]] = (*result)[lane];
    part.done = true; trace(entry, "tensor_memory_complete");
    return true;
}
}

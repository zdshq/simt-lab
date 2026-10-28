#include "simt/async.hpp"
#include <stdexcept>

namespace simt {
// Capture source bytes on issue, but defer their visibility at the destination.
// 发起时捕获源字节，但延迟它们在目标位置的可见性。
void issue_copy(const ExecutionContext& context, uint32_t lane) {
    const auto& i = context.instruction;
    const auto source = read_register(context.before, i.ra, lane);
    const auto destination = read_register(context.before, i.rb, lane);
    const auto address = read_register(context.before, i.rc, lane);
    if (!i.imm || i.imm > 4096) throw std::runtime_error("invalid_async_copy_size");
    auto& barrier = transaction(context.cta, address);
    if (i.imm > barrier.bytes - barrier.issued) throw std::runtime_error("async_exceeds_expected_bytes");
    context.global.check(source, i.imm);
    context.cta.shared.check(destination, i.imm);
    check_shared_write(context.cta, destination, i.imm);
    AsyncCopy copy{destination, address, barrier.phase, 3, {}};
    for (uint32_t offset = 0; offset < i.imm; ++offset)
        copy.bytes.push_back(static_cast<uint8_t>(context.global.read(uint64_t{source} + offset, 1)));
    barrier.issued += i.imm;
    context.cta.pending_copies.push_back(std::move(copy));
    update_transaction(context.cta, address);
}

namespace {
// Completion writes payload bytes before publishing transaction completion.
// 完成操作先写入载荷字节，再发布事务完成状态。
void complete_copy(Cta& cta, const AsyncCopy& copy) {
    auto& barrier = transaction(cta, copy.barrier);
    if (barrier.phase != copy.phase) throw std::runtime_error("async_phase_mismatch");
    for (uint32_t offset = 0; offset < copy.bytes.size(); ++offset)
        cta.shared.write(uint64_t{copy.destination} + offset, 1, copy.bytes[offset]);
    barrier.bytes -= static_cast<uint32_t>(copy.bytes.size());
    barrier.issued -= static_cast<uint32_t>(copy.bytes.size());
    update_transaction(cta, copy.barrier);
}
}

// Opportunities are deterministic event deferrals, never a prediction of GPU cycles.
// 调度机会仅表示确定的事件延迟，不预测 GPU 周期。
bool progress_async(Cta& cta) {
    const bool progress = !cta.pending_copies.empty();
    auto event = cta.pending_copies.begin();
    while (event != cta.pending_copies.end()) {
        if (--event->opportunities) { ++event; continue; }
        complete_copy(cta, *event);
        event = cta.pending_copies.erase(event);
    }
    release_async_waiters(cta);
    return progress;
}
}

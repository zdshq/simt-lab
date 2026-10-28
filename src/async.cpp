#include "simt/async.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
// Reject access to an uninitialized shared-memory transaction barrier.
// 拒绝访问尚未初始化的 shared 事务屏障。
void initialize(const ExecutionContext& context, uint32_t address) {
    const auto expected = context.instruction.imm;
    if (!expected || expected > 1024) throw std::runtime_error("invalid_mbarrier_count");
    context.cta.shared.check(address, 32, 16);
    check_shared_write(context.cta, address, 32);
    for (const auto& copy : context.cta.pending_copies)
        if (address < uint64_t{copy.destination} + copy.bytes.size() && copy.destination < uint64_t{address} + 32)
            throw std::runtime_error("mbarrier_overlaps_pending_copy");
    context.cta.transactions.emplace(address, TransactionBarrier{0, expected, expected, 0, 0});
    update_transaction(context.cta, address);
}

// Byte expectations must be declared before arrivals can complete a phase.
// 必须在到达操作可能完成阶段之前声明预期字节数。
void expect_bytes(const ExecutionContext& context, uint32_t address) {
    auto& barrier = transaction(context.cta, address);
    const auto bytes = context.instruction.imm;
    if (!bytes || bytes > UINT32_MAX - barrier.bytes) throw std::runtime_error("invalid_expected_bytes");
    barrier.bytes += bytes;
    update_transaction(context.cta, address);
}

// Return the current phase token before arrival potentially advances the phase.
// 在到达可能推进阶段之前，返回当前阶段令牌。
void arrive(const ExecutionContext& context, uint32_t address, uint32_t lane) {
    auto& barrier = transaction(context.cta, address);
    if (!barrier.arrivals) throw std::runtime_error("mbarrier_arrival_overflow");
    write_register(context.after, context.instruction.rd, lane, barrier.phase);
    --barrier.arrivals;
    update_transaction(context.cta, address);
}

// A wait blocks only while its recorded phase has not yet completed.
// 仅在记录的阶段尚未完成时阻塞等待。
void wait(const ExecutionContext& context, uint32_t mask) {
    const auto& i = context.instruction;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(mask & lane_bit(lane))) continue;
        const auto address = read_register(context.before, i.ra, lane);
        const auto phase = read_register(context.before, i.rb, lane);
        const auto& barrier = transaction(context.cta, address);
        if (phase > barrier.phase) throw std::runtime_error("mbarrier_future_phase");
        if (phase < barrier.phase) continue;
        context.after.state[lane] = LaneState::async_barrier;
        context.after.blocked_token[lane] = address;
        context.after.waiting_phase[lane] = phase;
    }
}
}

// Shared addresses identify transaction barriers independently of named CTA barriers.
// shared 地址标识事务屏障，与 CTA 命名屏障相互独立。
TransactionBarrier& transaction(Cta& cta, uint32_t address) {
    const auto found = cta.transactions.find(address);
    if (found == cta.transactions.end()) throw std::runtime_error("mbarrier_uninitialized");
    return found->second;
}

// Reflect control state in a reserved 32-byte shared-memory record.
// 将控制状态同步到保留的 32 字节 shared 记录。
void update_transaction(Cta& cta, uint32_t address) {
    auto& barrier = transaction(cta, address);
    if (!barrier.arrivals && !barrier.bytes) {
        if (barrier.phase == UINT32_MAX) throw std::runtime_error("mbarrier_phase_overflow");
        ++barrier.phase;
        barrier.arrivals = barrier.expected;
    }
    const std::array<uint32_t, 8> words{0x3152424D, barrier.phase, barrier.expected,
                                      barrier.arrivals, barrier.bytes, barrier.issued, 0, 0};
    for (uint32_t index = 0; index < words.size(); ++index) cta.shared.write(address + index * 4, 4, words[index]);
}

// Protect the opaque barrier record from ordinary stores and overlapping copies.
// 保护不透明屏障记录，禁止普通写入与重叠搬运。
void check_shared_write(const Cta& cta, uint64_t address, uint64_t size) {
    for (const auto& [at, barrier] : cta.transactions) {
        static_cast<void>(barrier);
        if (address < uint64_t{at} + 32 && uint64_t{at} < address + size)
            throw std::runtime_error("write_overlaps_mbarrier");
    }
}

// Wake only threads waiting for phases that have actually completed.
// 只唤醒等待已实际完成阶段的线程。
void release_async_waiters(Cta& cta) {
    for (auto& warp : cta.warps)
        for (uint32_t lane = 0; lane < lanes; ++lane)
            if (warp.state[lane] == LaneState::async_barrier &&
                warp.waiting_phase[lane] < transaction(cta, warp.blocked_token[lane]).phase)
                warp.state[lane] = LaneState::active;
}

// Management commands have one issuer; waits may involve any subset of lanes.
// 管理命令只有一个发起线程；等待可涉及任意线程子集。
bool execute_async(const ExecutionContext& context, uint32_t mask) {
    const auto& i = context.instruction;
    if (i.op == Op::MB_WAIT) { wait(context, mask); return true; }
    if (i.op != Op::MB_INIT && i.op != Op::MB_EXPECT && i.op != Op::MB_ARRIVE && i.op != Op::CP_ASYNC) return false;
    if (!mask) return true;
    if (std::popcount(mask) != 1) throw std::runtime_error("async_single_issuer_required");
    const auto lane = static_cast<uint32_t>(std::countr_zero(mask));
    const auto address = read_register(context.before, i.ra, lane);
    if (i.op == Op::MB_INIT) initialize(context, address);
    if (i.op == Op::MB_EXPECT) expect_bytes(context, address);
    if (i.op == Op::MB_ARRIVE) arrive(context, address, lane);
    if (i.op == Op::CP_ASYNC) issue_copy(context, lane);
    return true;
}
}

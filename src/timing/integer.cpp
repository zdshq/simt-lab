#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
namespace {
// Guard the RTL's ISA v1 opcode assignments against changes to the canonical ISA generator.
// 检查 RTL 的 ISA v1 操作码分配，防止规范 ISA 生成器变更后静默错配。
constexpr bool integer_encoding_matches() {
    constexpr std::array operations{Op::NOP, Op::MOVI, Op::MOV, Op::S2R, Op::IADD, Op::ISUB, Op::IMUL,
        Op::IMAD, Op::AND, Op::OR, Op::XOR, Op::SHL, Op::SHR, Op::SAR, Op::ISETP};
    for (uint32_t index = 0; index < operations.size(); ++index)
        if (static_cast<uint32_t>(operations[index]) != index) return false;
    return static_cast<uint32_t>(Op::CVTA) == 43;
}
static_assert(integer_encoding_matches());
}

// No host vector is read or computed: collector acceptance is the circuit's dispatch handshake.
// 不读取或计算宿主向量：收集器接收行为就是电路的分派握手。
bool TimingMachine::launch_integer(Inflight& entry) {
    auto* unit = sms_[entry.sm]->partitions[entry.partition].registers->integer();
    if (!unit || !entry.completion) throw std::runtime_error("integer_rtl_missing_completion_path");
    const auto& i = entry.instruction;
    const hw::IntegerCommand command{*entry.collector, *entry.completion, static_cast<uint32_t>(i.op), i.mode,
        i.ra, i.rb, i.rc, entry.mask, i.imm, machine_.config_.pipeline.integer_latency,
        entry.warp, entry.cta, entry.sm, machine_.config_.threads, machine_.config_.blocks};
    if (!unit->launch(command)) return false;
    release_collector_owner(entry);
    entry.stage = Stage::integer;
    return true;
}

// Completion readiness comes from physical storage and its arriving-result bypass, never a host due time.
// 完成就绪来自物理存储及其到达结果旁路，而不是宿主到期时间。
void TimingMachine::poll_rtl_execution(Inflight& entry) {
    auto* unit = sms_[entry.sm]->partitions[entry.partition].registers->writeback();
    if (!unit->completed(*entry.completion)) return;
    entry.result_submitted = true;
    entry.stage = Stage::writeback;
}
}

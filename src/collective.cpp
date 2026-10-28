#include "simt/control.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
// Every named member must execute the same dynamic collective instruction.
// 每个被指定的成员必须执行同一条动态集合指令。
uint32_t members(const Instruction& instruction, uint32_t mask) {
    if (!mask) return 0;
    if (!instruction.imm) throw std::runtime_error("empty_collective_members");
    if (instruction.imm & ~mask) throw std::runtime_error("membermask_not_subset");
    return instruction.imm;
}

// Elect one leader while leaving nonmember predicate bits untouched.
// 选出一个 leader，同时保持非成员谓词位不变。
void elect(const ExecutionContext& context, uint32_t mask) {
    const auto member_mask = members(context.instruction, mask);
    if (!member_mask) return;
    const auto leader = static_cast<uint32_t>(std::countr_zero(member_mask));
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (member_mask & lane_bit(lane))
            write_predicate(context.after, context.instruction.rd, lane, lane == leader);
}

// VOTE modes return either a ballot mask or a Boolean integer.
// VOTE 模式返回投票位掩码或布尔整数。
uint32_t vote_value(const ExecutionContext& context, uint32_t member_mask) {
    const auto& i = context.instruction;
    if (i.ra >= 8) throw std::runtime_error("predicate_out_of_range");
    const auto ballot = context.before.predicates[i.ra] & member_mask;
    switch (i.mode) {
    case 0: return ballot;
    case 1: return ballot != 0;
    case 2: return ballot == member_mask;
    case 3: return ballot == 0 || ballot == member_mask;
    default: throw std::runtime_error("invalid_vote_mode");
    }
}

// Broadcast the reduction only to participating members.
// 仅向参与成员广播归约结果。
void vote(const ExecutionContext& context, uint32_t mask) {
    const auto member_mask = members(context.instruction, mask);
    if (!member_mask) return;
    const auto value = vote_value(context, member_mask);
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (member_mask & lane_bit(lane)) write_register(context.after, context.instruction.rd, lane, value);
}

// Shuffle reads the pre-step register file, including when source equals destination.
// SHFL 从执行前寄存器堆读取，即使源寄存器和目标寄存器相同。
void shuffle(const ExecutionContext& context, uint32_t mask) {
    const auto member_mask = members(context.instruction, mask);
    const auto& i = context.instruction;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(member_mask & lane_bit(lane))) continue;
        const auto source = read_register(context.before, i.rb, lane);
        if (source >= lanes || !(member_mask & lane_bit(source)))
            throw std::runtime_error("shuffle_source_not_member");
        write_register(context.after, i.rd, lane, read_register(context.before, i.ra, source));
    }
}

// Report the active PC group before predicate filtering.
// 返回谓词筛选之前的活跃 PC 线程组。
void active_mask(const ExecutionContext& context, Group group, uint32_t mask) {
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (mask & lane_bit(lane)) write_register(context.after, context.instruction.rd, lane, group.mask);
}
}

// Keep cross-lane operations separate from per-lane ALU execution.
// 将跨线程操作与逐线程 ALU 执行分开。
bool execute_collective(const ExecutionContext& context, Group group, uint32_t mask) {
    switch (context.instruction.op) {
    case Op::ELECT: elect(context, mask); return true;
    case Op::VOTE: vote(context, mask); return true;
    case Op::SHFL: shuffle(context, mask); return true;
    case Op::ACTIVEMASK: active_mask(context, group, mask); return true;
    default: return false;
    }
}
}

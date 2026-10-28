#include "simt/execution.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace simt {
uint32_t lane_bit(uint32_t lane) { return uint32_t{1} << lane; }
float to_float(uint32_t value) { return std::bit_cast<float>(value); }

// Normalize NaNs to make snapshot payload bits reproducible.
// 规范化 NaN，使快照中的负载位可复现。
uint32_t float_bits(float value) {
    return std::isnan(value) ? 0x7fc00000u : std::bit_cast<uint32_t>(value);
}

// Read R63 as zero regardless of previous writes.
// 无论此前如何写入，读取 R63 总是返回零。
uint32_t read_register(const Warp& warp, uint32_t reg, uint32_t lane) {
    if (reg >= registers) throw std::runtime_error("register_out_of_range");
    return reg == zero_register ? 0u : warp.r[reg][lane];
}

// Validate the destination and discard writes to R63.
// 检查目标编号，并丢弃对 R63 的写入。
void write_register(Warp& warp, uint32_t reg, uint32_t lane, uint32_t value) {
    if (reg >= registers) throw std::runtime_error("register_out_of_range");
    if (reg != zero_register) warp.r[reg][lane] = value;
}

// Preserve predicate bits for lanes outside this instruction.
// 保留未参与当前指令的线程所持有的谓词位。
void write_predicate(Warp& warp, uint32_t pred, uint32_t lane, bool value) {
    if (pred >= 7) throw std::runtime_error("predicate_out_of_range");
    warp.predicates[pred] = (warp.predicates[pred] & ~lane_bit(lane)) | (value ? lane_bit(lane) : 0u);
}

// Predication filters only the current PC group.
// 谓词只筛选当前 PC 线程组。
uint32_t participating_mask(const Warp& warp, const Instruction& instruction, Group group) {
    uint32_t mask = warp.predicates[instruction.guard & 7];
    if (instruction.guard & 8) mask = ~mask;
    return mask & group.mask;
}

namespace {
int32_t signed_value(uint32_t value) { return std::bit_cast<int32_t>(value); }

// Implement sign extension explicitly for portable arithmetic shifts.
// 显式实现符号扩展，保证算术移位可移植。
uint32_t arithmetic_shift(uint32_t value, uint32_t shift) {
    shift &= 31;
    if (shift == 0) return value;
    const uint32_t high = (value & 0x80000000u) ? (~uint32_t{0} << (32 - shift)) : 0;
    return (value >> shift) | high;
}

// Arithmetic wraps modulo 2^32; shifts use the low five count bits.
// 算术按 2^32 取模；移位量使用低五位。
uint32_t integer_result(Op op, uint32_t a, uint32_t b, uint32_t c) {
    switch (op) {
    case Op::IADD: return a + b;
    case Op::ISUB: return a - b;
    case Op::IMUL: return a * b;
    case Op::IMAD: return a * b + c;
    case Op::AND: return a & b;
    case Op::OR: return a | b;
    case Op::XOR: return a ^ b;
    case Op::SHL: return a << (b & 31);
    case Op::SHR: return a >> (b & 31);
    case Op::SAR: return arithmetic_shift(a, b);
    default: throw std::runtime_error("not_integer_operation");
    }
}

// Reject undefined C++ float-to-integer conversions.
// 拒绝会触发未定义行为的 C++ 浮点转整数操作。
uint32_t float_to_integer(float value) {
    const double truncated = std::trunc(static_cast<double>(value));
    if (!std::isfinite(value) || truncated < -2147483648.0 || truncated > 2147483647.0)
        throw std::runtime_error("float_to_integer_out_of_range");
    return std::bit_cast<uint32_t>(static_cast<int32_t>(truncated));
}

// SFU functions use the host library as specified in docs/ISA.md.
// SFU 函数按 docs/ISA.md 的约定使用宿主数学库。
uint32_t float_result(Op op, uint32_t a, uint32_t b, uint32_t c) {
    switch (op) {
    case Op::FADD: return float_bits(to_float(a) + to_float(b));
    case Op::FMUL: return float_bits(to_float(a) * to_float(b));
    case Op::FFMA: return float_bits(std::fma(to_float(a), to_float(b), to_float(c)));
    case Op::FEXP2: return float_bits(std::exp2(to_float(a)));
    case Op::FRCP: return float_bits(1.0f / to_float(a));
    case Op::FRSQRT: return float_bits(1.0f / std::sqrt(to_float(a)));
    case Op::I2F: return float_bits(static_cast<float>(signed_value(a)));
    case Op::F2I: return float_to_integer(to_float(a));
    default: throw std::runtime_error("not_float_operation");
    }
}

// Interpret comparison selectors using the project's ISA.
// 按项目 ISA 解释比较选择子。
bool integer_compare(uint32_t a, uint32_t b, uint32_t mode) {
    switch (mode) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a <= b;
    case 4: return signed_value(a) < signed_value(b);
    case 5: return signed_value(a) <= signed_value(b);
    default: throw std::runtime_error("invalid_integer_comparison");
    }
}

// Keep IEEE unordered comparison behavior for NaNs.
// 对 NaN 保留 IEEE 无序比较行为。
bool float_compare(float a, float b, uint32_t mode) {
    switch (mode) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a <= b;
    default: throw std::runtime_error("invalid_float_comparison");
    }
}

// Report logical identifiers and actual simulated block placement.
// 返回逻辑标识和实际的模拟线程块位置。
uint32_t special_register(const ExecutionContext& context, uint32_t lane) {
    switch (context.instruction.imm) {
    case 0: return lane;
    case 1: return context.warp_id * lanes + lane;
    case 2: return context.warp_id;
    case 3: return context.cta.block_id;
    case 4: return context.cta.sm_id;
    case 5: return context.config.threads;
    case 6: return context.config.blocks;
    default: throw std::runtime_error("unknown_special_register");
    }
}

// Read operands from the immutable pre-step state.
// 从不可变的指令执行前状态读取操作数。
uint32_t scalar_result(const ExecutionContext& context, uint32_t lane) {
    const auto& i = context.instruction;
    if (i.op == Op::MOVI) return i.imm;
    if (i.op == Op::S2R) return special_register(context, lane);
    const auto a = read_register(context.before, i.ra, lane);
    if (i.op == Op::MOV) return a;
    const auto b = read_register(context.before, i.rb, lane);
    const auto c = read_register(context.before, i.rc, lane);
    if (i.op >= Op::IADD && i.op <= Op::SAR) return integer_result(i.op, a, b, c);
    return float_result(i.op, a, b, c);
}

// Update only the current lane's predicate bit.
// 只更新当前线程的谓词位。
void execute_comparison(const ExecutionContext& context, uint32_t lane) {
    const auto& i = context.instruction;
    const auto a = read_register(context.before, i.ra, lane);
    const auto b = read_register(context.before, i.rb, lane);
    const bool yes = i.op == Op::ISETP ? integer_compare(a, b, i.mode) : float_compare(to_float(a), to_float(b), i.mode);
    write_predicate(context.after, i.rd, lane, yes);
}
}

// Separate state-changing operations from ordinary register results.
// 将状态变化操作与普通寄存器结果分开处理。
void execute_scalar_lane(const ExecutionContext& context, uint32_t lane) {
    const auto& i = context.instruction;
    if (i.op == Op::NOP) return;
    if (i.op == Op::EXIT) { context.after.state[lane] = LaneState::exited; return; }
    if (i.op == Op::ISETP || i.op == Op::FSETP) { execute_comparison(context, lane); return; }
    write_register(context.after, i.rd, lane, scalar_result(context, lane));
}
}

#include "simt/memsync.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
constexpr uint32_t shared_base = 0xF0000000;
constexpr uint32_t local_base = 0xF1000000;
struct Access {
    const Memory* source;
    Memory* destination;
    uint64_t address;
    uint32_t lane;
    uint32_t value;
};

// Address arithmetic traps on overflow instead of silently wrapping to zero.
// 地址计算溢出时报告错误，不静默绕回零地址。
uint64_t address(const ExecutionContext& context, uint32_t lane) {
    const auto& i = context.instruction;
    const auto result = int64_t{read_register(context.before, i.ra, lane)} + std::bit_cast<int32_t>(i.imm);
    if (result < 0 || result > UINT32_MAX) throw std::runtime_error("memory_address_overflow");
    return static_cast<uint64_t>(result);
}

// Direct instructions select their space before address translation.
// 直达指令在地址转换之前选择存储空间。
uint32_t space(Op op) {
    switch (op) {
    case Op::LDG: case Op::STG: return 0;
    case Op::LDS: case Op::STS: return 1;
    case Op::LDL: case Op::STL: return 2;
    case Op::LDC: return 3;
    default: return 4;
    }
}

// Generic windows retain per-CTA shared and per-thread local ownership.
// 通用窗口保留 shared 的 CTA 归属和 local 的逐线程归属。
Access resolve(const ExecutionContext& context, uint32_t lane, uint32_t kind, uint64_t at) {
    if (kind == 4) {
        if (at >= shared_base && at < uint64_t{shared_base} + 65536) { kind = 1; at -= shared_base; }
        else if (at >= local_base && at < uint64_t{local_base} + 16384) { kind = 2; at -= local_base; }
        else kind = 0;
    }
    Memory* memory = nullptr;
    if (kind == 0) memory = &context.global;
    if (kind == 1) memory = &context.cta.shared;
    if (kind == 2) memory = &context.cta.local[context.warp_id * lanes + lane];
    if (kind == 3) return {&context.constant, nullptr, at, lane, 0};
    if (!memory) throw std::runtime_error("invalid_memory_space");
    return {memory, memory, at, lane, 0};
}

// Width and extension are explicit, including complete overwrite of high bits.
// 显式定义宽度与扩展方式，包括完全覆盖高位。
uint32_t width(uint32_t mode) {
    if (mode > 4) throw std::runtime_error("invalid_load_store_mode");
    return mode < 2 ? 1 : mode < 4 ? 2 : 4;
}

// Sign extension avoids signed shifts and host-dependent integer conversion.
// 符号扩展避免有符号移位和依赖宿主的整数转换。
uint32_t extended(uint32_t value, uint32_t mode) {
    if (mode == 1 && (value & 0x80)) return value | 0xFFFFFF00;
    if (mode == 3 && (value & 0x8000)) return value | 0xFFFF0000;
    return value;
}

// Validate every scatter target before committing any store.
// 在提交任何写入之前检查全部 scatter 目标。
std::vector<Access> accesses(const ExecutionContext& context, uint32_t mask, uint32_t kind, uint32_t size) {
    std::vector<Access> result;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(mask & lane_bit(lane))) continue;
        auto access = resolve(context, lane, kind, address(context, lane));
        access.source->check(access.address, size, size);
        access.value = read_register(context.before, context.instruction.rb, lane);
        result.push_back(access);
    }
    return result;
}

// Loads gather pre-step bytes; stores commit in ascending lane order.
// 读取收集执行前字节；写入按 lane 编号升序提交。
void load_store(const ExecutionContext& context, uint32_t mask, bool store) {
    const auto& i = context.instruction;
    const auto size = width(i.mode);
    for (const auto& access : accesses(context, mask, space(i.op), size)) {
        if (store) access.destination->write(access.address, size, access.value);
        else write_register(context.after, i.rd, access.lane,
                            extended(access.source->read(access.address, size), i.mode));
    }
}

// Atomic modes use wrapping integer arithmetic and explicitly signed comparisons.
// 原子操作采用整数取模运算和显式有符号比较。
uint32_t atomic_result(uint32_t mode, uint32_t old, uint32_t value, uint32_t replacement) {
    switch (mode) {
    case 0: return old + value;
    case 1: return value;
    case 2: return old == value ? replacement : old;
    case 3: return old & value;
    case 4: return old | value;
    case 5: return old ^ value;
    case 6: return std::min(old, value);
    case 7: return std::max(old, value);
    case 8: return std::bit_cast<int32_t>(old) < std::bit_cast<int32_t>(value) ? old : value;
    case 9: return std::bit_cast<int32_t>(old) > std::bit_cast<int32_t>(value) ? old : value;
    default: throw std::runtime_error("invalid_atomic_mode");
    }
}

// Each atomic lane performs a complete read-modify-write before the next lane.
// 每个原子线程在下一线程执行前完成完整的读改写。
void atomic(const ExecutionContext& context, uint32_t mask) {
    const auto& i = context.instruction;
    if (i.aux > 1) throw std::runtime_error("invalid_atomic_space");
    if (i.mode > 9) throw std::runtime_error("invalid_atomic_mode");
    for (const auto& access : accesses(context, mask, i.aux, 4)) {
        const auto replacement = read_register(context.before, i.rc, access.lane);
        const auto old = access.source->read(access.address, 4);
        if (i.op == Op::ATOM) write_register(context.after, i.rd, access.lane, old);
        access.destination->write(access.address, 4, atomic_result(i.mode, old, access.value, replacement));
    }
}

// CVTA changes address representation without accessing memory.
// CVTA 只改变地址表示，不访问内存。
void convert_address(const ExecutionContext& context, uint32_t mask) {
    const auto& i = context.instruction;
    if (i.mode > 3) throw std::runtime_error("invalid_cvta_mode");
    const uint32_t base = i.mode % 2 ? local_base : shared_base;
    const uint32_t limit = i.mode % 2 ? 16384 : 65536;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(mask & lane_bit(lane))) continue;
        const auto source = read_register(context.before, i.ra, lane);
        const auto offset = i.mode < 2 ? uint64_t{source} : uint64_t{source} - base;
        if (offset >= limit) throw std::runtime_error("cvta_address_out_of_range");
        write_register(context.after, i.rd, lane, i.mode < 2 ? source + base : source - base);
    }
}
}

// Serial commits already provide the ordering represented by a functional fence.
// 串行提交已经提供功能层 fence 所表示的排序。
bool execute_memory(const ExecutionContext& context, uint32_t mask) {
    switch (context.instruction.op) {
    case Op::LDG: case Op::LDS: case Op::LDL: case Op::LDC: case Op::LD:
        load_store(context, mask, false); return true;
    case Op::STG: case Op::STS: case Op::STL: case Op::ST:
        load_store(context, mask, true); return true;
    case Op::ATOM: case Op::RED: atomic(context, mask); return true;
    case Op::CVTA: convert_address(context, mask); return true;
    case Op::FENCE:
        if (context.instruction.mode > 2 || context.instruction.aux > 2)
            throw std::runtime_error("invalid_fence_scope_or_order");
        return true;
    default: return false;
    }
}
}

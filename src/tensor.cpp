#include "simt/tensor.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
// Allocation and MMA commands are issued by exactly one participating thread.
// 分配与 MMA 命令由恰好一个参与线程发起。
uint32_t issuer(uint32_t mask) {
    if (std::popcount(mask) != 1) throw std::runtime_error("tensor_single_issuer_required");
    return static_cast<uint32_t>(std::countr_zero(mask));
}

// Allocate contiguous columns and initialize every cell in the allocation.
// 分配连续列，并初始化分配区域中的每个单元。
uint32_t allocate(Cta& cta, uint32_t count) {
    if (!count || count > 256 || count % 8) throw std::runtime_error("invalid_tensor_allocation");
    for (uint32_t base = 0; base + count <= 256; base += 8) {
        bool available = true;
        for (uint32_t column = base; column < base + count; ++column)
            available &= !cta.tensor_columns[column];
        if (!available) continue;
        for (uint32_t column = base; column < base + count; ++column) {
            cta.tensor_columns.set(column);
            for (uint32_t row = 0; row < 128; ++row) cta.tmem.write((column * 128 + row) * 4, 4, 0);
        }
        cta.tensor_allocations[base] = count;
        return base;
    }
    throw std::runtime_error("tensor_out_of_columns");
}

// Free requires the exact original allocation rather than an overlapping subrange.
// 释放必须对应完整原始分配，不能释放重叠子区间。
void free_columns(Cta& cta, uint32_t base, uint32_t count) {
    const auto allocation = cta.tensor_allocations.find(base);
    if (allocation == cta.tensor_allocations.end() || allocation->second != count)
        throw std::runtime_error("invalid_tensor_free");
    for (uint32_t column = base; column < base + count; ++column) cta.tensor_columns.reset(column);
    cta.tensor_allocations.erase(allocation);
}

// Coordinate loads and stores expose accumulator cells to ordinary lane registers.
// 坐标式读写将累加器单元与普通线程寄存器连接。
void transfer(const ExecutionContext& context, uint32_t mask) {
    const auto& i = context.instruction;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(mask & lane_bit(lane))) continue;
        const auto at = tensor_address(context.cta, read_register(context.before, i.ra, lane),
                                       read_register(context.before, i.rb, lane));
        if (i.op == Op::LDT) write_register(context.after, i.rd, lane, context.cta.tmem.read(at, 4));
        else context.cta.tmem.write(at, 4, read_register(context.before, i.rc, lane));
    }
}
}

// Column-major addressing matches 128 rows of 32-bit cells per column.
// 列优先寻址对应每列 128 行、每个单元 32 位的组织。
uint32_t tensor_address(const Cta& cta, uint32_t row, uint32_t column) {
    if (row >= 128 || column >= 256) throw std::runtime_error("tensor_coordinate_out_of_range");
    if (!cta.tensor_columns[column]) throw std::runtime_error("tensor_column_not_allocated");
    return (column * 128 + row) * 4;
}

// Keep matrix issue separate from lane-wise TMEM transfers.
// 将矩阵命令发起与逐线程 TMEM 搬运分开。
bool execute_tensor(const ExecutionContext& context, uint32_t mask) {
    const auto& i = context.instruction;
    if (i.op == Op::LDT || i.op == Op::STT) { transfer(context, mask); return true; }
    if (i.op != Op::TMEM_ALLOC && i.op != Op::TMEM_FREE && i.op != Op::MMA) return false;
    if (!mask) return true;
    const auto lane = issuer(mask);
    if (i.op == Op::TMEM_ALLOC) write_register(context.after, i.rd, lane, allocate(context.cta, i.imm));
    if (i.op == Op::TMEM_FREE) free_columns(context.cta, read_register(context.before, i.ra, lane), i.imm);
    if (i.op == Op::MMA) matrix_multiply(context, read_register(context.before, i.ra, lane));
    return true;
}
}

#pragma once
#include "simt/hardware_memory.hpp"
#include "simt/hardware_tensor.hpp"
#include "simt/tensor.hpp"

namespace simt {
enum class TensorPhase { start, zero, transfer, descriptor, initial, metadata, operands, feed, drain, store, complete };
struct TensorTransfer {
    uint32_t space;
    hw::WarpAccess access;
    std::array<uint32_t, 32> slots{};
    std::optional<hw::Ticket> ticket;
    bool done = false;
};

// Staging stores one microtile and one operand wave, never an entire matrix result computed early.
// 暂存仅保存一个微 tile 和一组操作数，不提前计算整个矩阵结果。
struct TensorFlight {
    TensorPhase phase = TensorPhase::start;
    MatrixDescriptor descriptor{};
    uint32_t row = 0, column = 0, rows = 0, columns = 0, k = 0, base = 0, cursor = 0, end = 0;
    std::array<uint64_t, 4> indices_a{};
    std::array<uint32_t, 32> fetched{};
    std::vector<TensorTransfer> transfers;
    hw::TensorWave wave;
};

// A transfer's slot routes returned bytes into the frontend's bounded staging registers.
// 事务 slot 将返回字节送入前端有限暂存寄存器。
void tensor_transfer(TensorFlight& flight, uint32_t space, uint64_t address, uint32_t width,
                     uint32_t slot, bool write = false, uint32_t value = 0);
}

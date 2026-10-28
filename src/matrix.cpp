#include "simt/tensor.hpp"
#include <cmath>
#include <stdexcept>

namespace simt {
namespace {
struct MatrixInput {
    const MatrixDescriptor& d;
    const Memory& memory;
    uint32_t row, column;
};

// Metadata maps each retained value back to its logical K coordinate.
// 元数据将每个保留值映射回逻辑 K 坐标。
uint32_t logical_k(const MatrixInput& input, uint32_t entry) {
    const auto& d = input.d;
    if (!(d.flags & 2)) return entry;
    const auto metadata = input.memory.read(uint64_t{d.metadata} + uint64_t{input.row} * d.stride_metadata + entry / 2, 1);
    const auto first = metadata & 3;
    const auto second = (metadata >> 2) & 3;
    if ((metadata & 0xF0) || first >= second) throw std::runtime_error("invalid_sparse_metadata");
    return (entry / 2) * 4 + (entry % 2 ? second : first);
}

// Fused binary32 accumulation follows increasing logical K order.
// 融合 binary32 累加按照逻辑 K 递增顺序执行。
float float_dot(const MatrixInput& input, uint32_t begin, uint32_t count, float accumulator) {
    const auto& d = input.d;
    for (uint32_t entry = begin; entry < begin + count; ++entry) {
        const auto k = logical_k(input, entry);
        const auto a = tensor_float(input.memory, d.a, uint64_t{input.row} * d.stride_a + entry, d.format);
        const auto b = tensor_float(input.memory, d.b, uint64_t{k} * d.stride_b + input.column, d.format);
        accumulator = std::fma(a, b, accumulator);
    }
    return accumulator;
}

// Signed INT8 products accumulate modulo 2^32 without signed overflow.
// 有符号 INT8 乘积按 2^32 取模累加，避免有符号溢出。
uint32_t integer_dot(const MatrixInput& input, uint32_t accumulator) {
    const auto& d = input.d;
    const auto count = d.flags & 2 ? d.k / 2 : d.k;
    for (uint32_t entry = 0; entry < count; ++entry) {
        const auto k = logical_k(input, entry);
        const auto a = tensor_integer(input.memory, d.a, uint64_t{input.row} * d.stride_a + entry);
        const auto b = tensor_integer(input.memory, d.b, uint64_t{k} * d.stride_b + input.column);
        accumulator += static_cast<uint32_t>(a * b);
    }
    return accumulator;
}

// Scale storage is binary32 and each scale must be finite and nonnegative.
// 缩放系数以 binary32 存储，必须有限且非负。
float scale(const Memory& memory, uint32_t base, uint64_t index) {
    const auto value = to_float(memory.read(uint64_t{base} + index * 4, 4));
    if (!std::isfinite(value) || value < 0) throw std::runtime_error("invalid_block_scale");
    return value;
}

// Round each block dot, multiply its scales, then fuse the scaled block into D.
// 对每块点积舍入，计算缩放系数乘积，再将缩放后的块融合累加到 D。
float scaled_dot(const MatrixInput& input, float accumulator) {
    const auto& d = input.d;
    const auto retained = d.flags & 2 ? d.block_k / 2 : d.block_k;
    for (uint32_t block = 0; block < d.k / d.block_k; ++block) {
        const auto dot = float_dot(input, block * retained, retained, 0.0F);
        const auto a = scale(input.memory, d.scale_a, uint64_t{input.row} * d.stride_scale_a + block);
        const auto b = scale(input.memory, d.scale_b, uint64_t{block} * d.stride_scale_b + input.column);
        const float product = a * b;
        accumulator = std::fma(dot, product, accumulator);
    }
    return accumulator;
}

// Preserve accumulator bits for integer MMA and convert them only for floating MMA.
// 整数 MMA 保留累加器位模式，浮点 MMA 才进行浮点解释。
uint32_t cell_result(const MatrixInput& input, uint32_t previous) {
    const auto& d = input.d;
    if (!(d.flags & 1)) previous = 0;
    if (d.format == 3) return integer_dot(input, previous);
    if (d.flags & 4) return float_bits(scaled_dot(input, to_float(previous)));
    return float_bits(float_dot(input, 0, d.flags & 2 ? d.k / 2 : d.k, to_float(previous)));
}
}

// Compute the entire tile before committing any TMEM result.
// 完整计算整个 tile 后，再提交任何 TMEM 结果。
void matrix_multiply(const ExecutionContext& context, uint32_t address) {
    const auto d = read_descriptor(context.cta, address);
    const auto& memory = d.space ? context.global : context.cta.shared;
    std::vector<std::pair<uint32_t, uint32_t>> result;
    for (uint32_t row = 0; row < d.m; ++row) {
        for (uint32_t column = 0; column < d.n; ++column) {
            const auto at = tensor_address(context.cta, d.row_d + row, d.column_d + column);
            const auto bits = cell_result({d, memory, row, column}, context.cta.tmem.read(at, 4));
            result.emplace_back(at, bits);
        }
    }
    for (const auto& [at, bits] : result) context.cta.tmem.write(at, 4, bits);
}
}

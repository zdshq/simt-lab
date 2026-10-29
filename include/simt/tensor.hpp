#pragma once
#include "simt/execution.hpp"

namespace simt {
struct MatrixDescriptor {
    uint32_t format, flags, m, n, k;
    uint32_t a, b, stride_a, stride_b, row_d, column_d;
    uint32_t metadata, stride_metadata, scale_a, scale_b, block_k, stride_scale_a, stride_scale_b, space;
};
bool execute_tensor(const ExecutionContext& context, uint32_t mask);
MatrixDescriptor read_descriptor(const Cta& cta, uint32_t address);
MatrixDescriptor decode_descriptor(const std::array<uint32_t, 20>& words);
uint32_t reserve_tensor_columns(Cta& cta, uint32_t count);
void free_tensor_columns(Cta& cta, uint32_t base, uint32_t count);
uint32_t tensor_address(const Cta& cta, uint32_t row, uint32_t column);
float tensor_float(const Memory& memory, uint32_t base, uint64_t index, uint32_t format);
float decode_tensor_float(uint32_t bits, uint64_t index, uint32_t format);
int32_t tensor_integer(const Memory& memory, uint32_t base, uint64_t index);
uint32_t tensor_element_size(uint32_t format);
void matrix_multiply(const ExecutionContext& context, uint32_t address);
}

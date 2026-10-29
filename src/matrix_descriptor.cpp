#include "simt/tensor.hpp"
#include <stdexcept>

namespace simt {
namespace {
// Bound dimensions and pitches before traversing descriptor-controlled memory.
// 遍历描述符控制的内存之前，限制维度和步长。
void validate(const MatrixDescriptor& d) {
    tensor_element_size(d.format);
    if (!d.m || d.m > 32 || !d.n || d.n > 32 || !d.k || d.k > 256)
        throw std::runtime_error("invalid_matrix_shape");
    if (d.flags & ~7U || d.space > 1) throw std::runtime_error("invalid_matrix_flags_or_space");
    const bool sparse = d.flags & 2;
    const bool scaled = d.flags & 4;
    if (sparse && (d.k % 4 || d.stride_metadata < d.k / 4))
        throw std::runtime_error("invalid_sparse_shape_or_pitch");
    if (d.stride_a < (sparse ? d.k / 2 : d.k) || d.stride_b < d.n)
        throw std::runtime_error("invalid_matrix_pitch");
    if (d.row_d >= 128 || d.m > 128 - d.row_d || d.column_d >= 256 || d.n > 256 - d.column_d)
        throw std::runtime_error("tensor_coordinate_out_of_range");
    if (scaled && (d.format == 3 || !d.block_k || d.block_k > d.k || d.k % d.block_k ||
                   d.stride_scale_a < d.k / d.block_k || d.stride_scale_b < d.n ||
                   (sparse && d.block_k % 4))) throw std::runtime_error("invalid_block_scaling");
}
}

// Descriptors are fixed little-endian records in CTA shared memory.
// 描述符是位于 CTA shared 中的固定小端记录。
MatrixDescriptor read_descriptor(const Cta& cta, uint32_t address) {
    cta.shared.check(address, 80, 16);
    std::array<uint32_t, 20> words{};
    for (uint32_t index = 0; index < words.size(); ++index) words[index] = cta.shared.read(address + 4 * index, 4);
    return decode_descriptor(words);
}

// Decode only returned bytes so timing frontends cannot bypass descriptor memory ports.
// 只解码已返回字节，避免时序前端绕过描述符内存端口。
MatrixDescriptor decode_descriptor(const std::array<uint32_t, 20>& words) {
    if (words[0] != 0x31414D4D) throw std::runtime_error("invalid_matrix_descriptor_magic");
    const MatrixDescriptor d{words[1], words[2], words[3], words[4], words[5], words[6], words[7],
                             words[8], words[9], words[10], words[11], words[12], words[13], words[14],
                             words[15], words[16], words[17], words[18], words[19]};
    validate(d);
    return d;
}
}

#include "simt/module.hpp"
#include <fstream>
#include <iterator>
#include <string>

namespace simt {
// Read the standalone CLI's raw binary input before decoding.
// 解码前读取独立 CLI 的原始二进制输入。
Module Module::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot_open_program");
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(stream), {}};
    return from_bytes(bytes);
}

// Decode once and report malformed encodings using instruction-index PCs.
// 只解码一次，并使用指令索引形式的 PC 报告畸形编码。
Module Module::from_bytes(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() % 16 != 0)
        throw std::runtime_error("invalid_program_size");
    Module module;
    for (size_t at = 0; at < bytes.size(); at += 16) {
        std::array<uint32_t, 4> words{};
        for (size_t w = 0; w < 4; ++w)
            for (size_t b = 0; b < 4; ++b)
                words[w] |= uint32_t(bytes[at + w * 4 + b]) << (8 * b);
        try {
            module.code.push_back(decode(words));
            module.image.push_back(words);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string(e.what()) + " at pc=" + std::to_string(at / 16));
        }
    }
    return module;
}
}

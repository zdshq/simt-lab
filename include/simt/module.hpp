#pragma once
#include "isa.hpp"
#include <filesystem>
#include <vector>

namespace simt {
struct Module {
    std::vector<Instruction> code;
    std::vector<std::array<uint32_t,4>> image;
    static Module load(const std::filesystem::path& path);
    static Module from_bytes(const std::vector<uint8_t>& bytes);
};
}

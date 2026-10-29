#pragma once
#include "isa.hpp"
#include <filesystem>
#include <vector>

namespace simt {
struct Module {
    std::vector<Instruction> code;
    static Module load(const std::filesystem::path& path);
    static Module from_bytes(const std::vector<uint8_t>& bytes);
};
}

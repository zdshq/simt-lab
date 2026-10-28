#pragma once
#include "isa.hpp"
#include <filesystem>
#include <vector>

namespace simt {
struct Module {
    std::vector<Instruction> code;
    static Module load(const std::filesystem::path& path);
};
}

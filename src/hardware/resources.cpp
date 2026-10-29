#include "simt/hardware_pipeline.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt::hw {
SmResources::SmResources(PipelineConfig config) : config_(config) {
    if (!config.max_warps || config.max_warps > 64 || !config.max_ctas || config.max_ctas > 64 ||
        !config.register_words || !config.shared_bytes || !config.partitions || config.partitions > 8 ||
        !config.collectors || config.collectors > 32 || !config.register_banks || config.register_banks > 64 ||
        !config.integer_latency || !config.float_latency || !config.special_latency ||
        !config.integer_interval || !config.float_interval || !config.special_interval)
        throw std::runtime_error("invalid_pipeline_config");
    slots_.resize(config.max_warps);
}

std::optional<Residency> SmResources::acquire(uint32_t cta, uint32_t threads, uint32_t shared_bytes) {
    if (!threads || threads > 1024 || shared_bytes > config_.shared_bytes || resident_.contains(cta))
        throw std::runtime_error("invalid_cta_resource_request");
    const auto warps = (threads + 31) / 32;
    Residency result{{}, warps * 32 * 64, (shared_bytes + 127) / 128 * 128};
    if (resident_.size() == config_.max_ctas || result.registers > config_.register_words - registers_ ||
        result.shared > config_.shared_bytes - shared_) return {};
    for (uint32_t slot = 0; slot < slots_.size() && result.slots.size() < warps; ++slot)
        if (!slots_[slot]) result.slots.push_back(slot);
    if (result.slots.size() != warps) return {};
    for (auto slot : result.slots) slots_[slot] = true;
    registers_ += result.registers; shared_ += result.shared;
    resident_.emplace(cta, result);
    return result;
}

void SmResources::release(uint32_t cta) {
    const auto found = resident_.find(cta);
    if (found == resident_.end()) throw std::runtime_error("unknown_resident_cta");
    for (auto slot : found->second.slots) slots_[slot] = false;
    registers_ -= found->second.registers;
    shared_ -= found->second.shared;
    resident_.erase(found);
}

RegisterPorts::RegisterPorts(uint32_t banks) : read_(banks), write_(banks) {
    if (!banks || banks > 64) throw std::runtime_error("invalid_register_banks");
}

bool RegisterPorts::claim(std::vector<std::optional<uint64_t>>& ports, uint32_t reg, uint64_t cycle) {
    if (reg == 63) return true;
    auto& used = ports[reg % ports.size()];
    if (used == cycle) return false;
    used = cycle;
    return true;
}

bool RegisterPorts::read(uint32_t reg, uint64_t cycle) { return claim(read_, reg, cycle); }
bool RegisterPorts::write(uint32_t reg, uint64_t cycle) { return claim(write_, reg, cycle); }
}

#pragma once
#include "simt/machine.hpp"

namespace simt {
// Preserve the source snapshot while evaluating all lanes of an instruction.
// 计算一条指令的所有线程时保持源状态快照不变。
struct ExecutionContext {
    const Instruction& instruction;
    Cta& cta;
    const Warp& before;
    Warp& after;
    const Config& config;
    uint32_t warp_id;
    Memory& global;
    const Memory& constant;
};

uint32_t lane_bit(uint32_t lane);
uint32_t read_register(const Warp& warp, uint32_t reg, uint32_t lane);
void write_register(Warp& warp, uint32_t reg, uint32_t lane, uint32_t value);
void write_predicate(Warp& warp, uint32_t pred, uint32_t lane, bool value);
uint32_t participating_mask(const Warp& warp, const Instruction& instruction, Group group);
float to_float(uint32_t value);
uint32_t float_bits(float value);
void execute_scalar_lane(const ExecutionContext& context, uint32_t lane);
}

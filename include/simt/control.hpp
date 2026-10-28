#pragma once
#include "simt/execution.hpp"

namespace simt {
bool resume_yielded(Warp& warp);
bool execute_control(const ExecutionContext& context, Group group, uint32_t mask, const Module& module);
bool execute_collective(const ExecutionContext& context, Group group, uint32_t mask);
}

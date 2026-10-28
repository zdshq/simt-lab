#pragma once
#include "simt/execution.hpp"

namespace simt {
bool execute_memory(const ExecutionContext& context, uint32_t mask);
bool execute_barrier(const ExecutionContext& context, uint32_t mask);
void release_cta_barriers(Cta& cta);
}

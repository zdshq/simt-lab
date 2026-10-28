#pragma once
#include "simt/execution.hpp"

namespace simt {
bool execute_async(const ExecutionContext& context, uint32_t mask);
bool progress_async(Cta& cta);
void release_async_waiters(Cta& cta);
void check_shared_write(const Cta& cta, uint64_t address, uint64_t size);
TransactionBarrier& transaction(Cta& cta, uint32_t address);
void update_transaction(Cta& cta, uint32_t address);
void issue_copy(const ExecutionContext& context, uint32_t lane);
}

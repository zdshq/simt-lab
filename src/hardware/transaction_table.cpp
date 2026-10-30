#include "simt/transaction_table.hpp"
#include <stdexcept>

namespace simt::hw {
const char* transaction_error(uint32_t code) {
    static constexpr std::array messages{"transaction_rejected","invalid_transaction_operation","invalid_transaction_capacity",
        "invalid_mbarrier_count","write_overlaps_mbarrier","mbarrier_table_full","mbarrier_uninitialized",
        "invalid_expected_bytes","mbarrier_arrival_overflow","invalid_async_copy_size","async_exceeds_expected_bytes",
        "async_phase_mismatch","async_accounting_underflow","mbarrier_phase_overflow","incomplete_async_transaction",
        "invalid_mbarrier_address"};
    return code<messages.size() ? messages[code] : "unknown_transaction_error";
}
#ifndef SIMT_HAS_REGISTER_RTL
std::unique_ptr<TransactionTable> make_transaction_table(uint32_t) { throw std::runtime_error("transaction_rtl_not_built"); }
#endif
}

#include "simt/transaction_table.hpp"
#include "Vsimt_transaction_table.h"
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlTransactionTable final : public TransactionTable {
public:
    explicit RtlTransactionTable(uint32_t capacity);
    ~RtlTransactionTable() override { dut_.final(); }
    bool available() const override { return dut_.command_ready && !dut_.command_valid; }
    void submit(const TransactionCommand& command) override;
    std::optional<TransactionResult> publication() const override;
    void acknowledge() override;
    std::optional<TransactionResult> take() override;
    uint32_t occupancy() const override { return dut_.occupancy; }
    void tick() override;
    const TransactionStats& stats() const override { return stats_; }
private:
    Vsimt_transaction_table dut_;
    TransactionStats stats_;
    TransactionResult result() const;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
};
RtlTransactionTable::RtlTransactionTable(uint32_t capacity) {
    if (!capacity || capacity>1024) throw std::out_of_range("invalid_transaction_capacity");
    dut_.clk=0; dut_.command_valid=0; dut_.publication_ack=0; dut_.response_ready=0;
    dut_.capacity=capacity; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}
void RtlTransactionTable::submit(const TransactionCommand& c) {
    if (!available()) throw std::runtime_error("transaction_command_phase");
    if (static_cast<uint32_t>(c.operation)>6) throw std::out_of_range("invalid_transaction_operation");
    dut_.operation=static_cast<uint32_t>(c.operation); dut_.cta=c.cta; dut_.address=c.address;
    dut_.amount=c.amount; dut_.completion_phase=c.phase; dut_.command_valid=1; dut_.eval();
}

// Output records are observations of stored circuit outputs, not a shadow transaction table.
// 输出记录是已存电路输出的观察值，不是影子事务表。
TransactionResult RtlTransactionTable::result() const {
    return {static_cast<TransactionOp>(dut_.result_operation),dut_.result_cta,dut_.result_address,dut_.arrival_token,dut_.error,
        {dut_.result_record[4],dut_.result_record[3],dut_.result_record[2],dut_.result_record[1],dut_.result_record[0]},
        bool(dut_.phase_advanced)};
}
std::optional<TransactionResult> RtlTransactionTable::publication() const {
    return dut_.publication_valid ? std::optional{result()} : std::nullopt;
}
void RtlTransactionTable::acknowledge() {
    if (!dut_.publication_valid || dut_.publication_ack) throw std::runtime_error("transaction_publication_phase");
    dut_.publication_ack=1; dut_.eval();
}
std::optional<TransactionResult> RtlTransactionTable::take() {
    if (!dut_.response_valid || dut_.response_ready) return {};
    auto response=result(); dut_.response_ready=1; dut_.eval(); return response;
}
void RtlTransactionTable::tick() {
    ++stats_.clocks;
    stats_.commands+=bool(dut_.command_valid && dut_.command_ready);
    stats_.searches+=dut_.observed_state==1;
    stats_.publication_stalls+=bool(dut_.publication_valid && !dut_.publication_ack);
    if (dut_.publication_valid && dut_.publication_ack) { ++stats_.publications; stats_.phases+=bool(dut_.phase_advanced); }
    if (dut_.response_valid && dut_.response_ready) { ++stats_.responses; stats_.errors+=dut_.error!=0; }
    if (dut_.clock_enable) { ++stats_.active_clocks; edge(); }
    dut_.command_valid=0; dut_.publication_ack=0; dut_.response_ready=0; dut_.eval();
}
}
std::unique_ptr<TransactionTable> make_transaction_table(uint32_t capacity) { return std::make_unique<RtlTransactionTable>(capacity); }
}

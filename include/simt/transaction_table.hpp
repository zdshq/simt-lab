#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace simt::hw {
enum class TransactionOp : uint32_t { initialize, expect, arrive, issue, complete, query, release };
struct TransactionCommand {
    TransactionOp operation;
    uint32_t cta = 0, address = 0, amount = 0, phase = 0;
};
struct TransactionRecord { uint32_t phase = 0, expected = 0, arrivals = 0, bytes = 0, issued = 0; };
struct TransactionResult {
    TransactionOp operation;
    uint32_t cta = 0, address = 0, token = 0, error = 0;
    TransactionRecord record;
    bool advanced = false;
};
struct TransactionStats {
    uint64_t clocks = 0, active_clocks = 0, commands = 0, searches = 0, publications = 0, responses = 0;
    uint64_t errors = 0, phases = 0, publication_stalls = 0;
};

// The interface accepts commands and publication acknowledgements, never host-computed next-state records.
// 接口接收命令和发布确认，绝不接收宿主计算的下一状态记录。
class TransactionTable {
public:
    virtual ~TransactionTable() = default;
    virtual bool available() const = 0;
    virtual void submit(const TransactionCommand& command) = 0;
    virtual std::optional<TransactionResult> publication() const = 0;
    virtual void acknowledge() = 0;
    virtual std::optional<TransactionResult> take() = 0;
    virtual uint32_t occupancy() const = 0;
    virtual void tick() = 0;
    virtual const TransactionStats& stats() const = 0;
};
std::unique_ptr<TransactionTable> make_transaction_table(uint32_t capacity);
const char* transaction_error(uint32_t code);
}

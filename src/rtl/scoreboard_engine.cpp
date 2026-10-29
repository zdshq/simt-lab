#include "simt/scoreboard_engine.hpp"
#include "Vsimt_scoreboard.h"
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlScoreboard final : public ScoreboardEngine {
public:
    RtlScoreboard() {
        dut_.clk = 0; dut_.reset = 1; dut_.command_valid = 0;
        edge(); dut_.reset = 0; dut_.eval();
    }
    ~RtlScoreboard() override { dut_.final(); }
    bool query(ScoreQuery kind, uint32_t reg, uint32_t mask) const override;
    void update(ScoreCommand command, uint32_t reg, uint32_t mask) override;
    void tick() override;
    uint64_t cycle() const override { return cycle_; }
    uint64_t active_cycles() const override { return active_cycles_; }
    std::string_view name() const override { return "rtl"; }
private:
    mutable Vsimt_scoreboard dut_;
    uint32_t pending_ = 0;
    uint64_t cycle_ = 0, active_cycles_ = 0;
    void edge() { dut_.clk = 1; dut_.eval(); dut_.clk = 0; dut_.eval(); }
    void set_register(uint32_t port, uint32_t reg);
};

// Pin packing changes combinational inputs only and never advances a hidden clock.
// 引脚打包只改变组合输入，不推进隐藏时钟。
void RtlScoreboard::set_register(uint32_t port, uint32_t reg) {
    const auto bit = port*6, word = bit/32, shift = bit%32;
    dut_.command_register[word] = (dut_.command_register[word] & ~(uint32_t{63} << shift)) | (reg << shift);
    if (shift > 26) {
        const auto count = shift-26, mask = (uint32_t{1} << count)-1;
        dut_.command_register[word+1] = (dut_.command_register[word+1] & ~mask) | (reg >> (32-shift));
    }
}

bool RtlScoreboard::query(ScoreQuery kind, uint32_t reg, uint32_t mask) const {
    if (reg >= (kind == ScoreQuery::predicate ? 8u : 64u)) throw std::out_of_range("scoreboard_query_register");
    if (reg == (kind == ScoreQuery::predicate ? 7u : 63u)) return true;
    if (dut_.query_register != reg || dut_.query_mask != mask) {
        dut_.query_register = reg; dut_.query_predicate = reg%8; dut_.query_mask = mask; dut_.eval();
    }
    if (kind == ScoreQuery::readable) return dut_.readable;
    if (kind == ScoreQuery::writable) return dut_.writable;
    return dut_.predicate_ready;
}

void RtlScoreboard::update(ScoreCommand command, uint32_t reg, uint32_t mask) {
    if (reg >= 64) throw std::out_of_range("scoreboard_command_register");
    if (reg == 63 && command <= ScoreCommand::retire_write) return;
    if (!mask && (command < ScoreCommand::reserve_predicate || reg < 7 ||
                  (command == ScoreCommand::retire_predicate && reg == 7))) return;
    if (pending_ == 16) throw std::runtime_error("scoreboard_rtl_event_capacity");
    set_register(pending_, reg);
    const auto shift = pending_*3;
    dut_.command = (dut_.command & ~(uint64_t{7} << shift)) | (uint64_t(command) << shift);
    dut_.command_mask[pending_] = mask;
    const auto bit = uint32_t{1} << pending_;
    dut_.command_valid |= bit; dut_.eval();
    if (!(dut_.command_ready & bit)) {
        const bool invalid = dut_.command_error & bit;
        dut_.command_valid &= ~bit; dut_.eval();
        throw std::runtime_error(invalid ? "scoreboard_rtl_invalid_transition" : "scoreboard_rtl_reader_capacity");
    }
    ++pending_;
}

void RtlScoreboard::tick() {
    ++cycle_;
    // A disabled RTL clock gate has no state transition; elide only that provably inert evaluation.
    // RTL 时钟门关闭时没有状态转换，只省略这个确定无效的求值。
    if (!dut_.clock_enable) return;
    edge(); ++active_cycles_;
    pending_ = 0; dut_.command_valid = 0; dut_.eval();
}
}

std::unique_ptr<ScoreboardEngine> make_rtl_scoreboard_engine() { return std::make_unique<RtlScoreboard>(); }
}

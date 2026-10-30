#include "simt/scoreboard_engine.hpp"
#include "simt/hardware_pipeline.hpp"
#include <stdexcept>

namespace simt::hw {
void validate_dependency_query(const DependencyQuery& q) {
    for (auto reg:q.registers) if (reg>=64) throw std::out_of_range("scoreboard_query_register");
    if (q.destination>=64 || q.predicate>=8 || q.source_predicate>=8)
        throw std::out_of_range("scoreboard_query_register");
}

// Observe one real issue query; categories can overlap and do not represent hardware counters.
// 观测一次真实发射查询；冲突类别可重叠，计数不属于硬件状态。
bool ScoreboardEngine::issue_ready(const DependencyQuery& query) {
    const auto result=dependencies(query); ++dependencies_.queries;
    dependencies_.blocked+=!result.ready; dependencies_.raw+=bool(result.blocked&7);
    dependencies_.waw+=bool(result.blocked&8); dependencies_.war+=bool(result.blocked&16);
    dependencies_.predicate+=bool(result.blocked&96); return result.ready;
}
namespace {
class ModelScoreboard final : public ScoreboardEngine {
public:
    bool query(ScoreQuery kind, uint32_t reg, uint32_t mask) const override {
        if (kind == ScoreQuery::readable) return state_.readable(reg, mask);
        if (kind == ScoreQuery::writable) return state_.writable(reg, mask);
        return state_.predicate_ready(reg, mask);
    }
    DependencyResult dependencies(const DependencyQuery& q) const override {
        validate_dependency_query(q); uint32_t blocked=0;
        for (uint32_t port=0; port<3; ++port)
            if (!state_.readable(q.registers[port],q.masks[port])) blocked|=1u<<port;
        if (!state_.readable(q.destination,q.write_mask)) blocked|=8;
        if (!state_.readers_clear(q.destination,q.write_mask)) blocked|=16;
        if (!state_.predicate_ready(q.predicate,q.predicate_mask)) blocked|=32;
        if (!state_.predicate_ready(q.source_predicate,q.source_predicate_mask)) blocked|=64;
        return {blocked==0,blocked};
    }
    void update(ScoreCommand command, uint32_t reg, uint32_t mask) override {
        switch (command) {
        case ScoreCommand::reserve_read: state_.reserve_read(reg, mask); break;
        case ScoreCommand::collect_read: state_.collect_read(reg, mask); break;
        case ScoreCommand::reserve_write: state_.reserve_write(reg, mask); break;
        case ScoreCommand::retire_write: state_.retire_write(reg, mask); break;
        case ScoreCommand::reserve_predicate: state_.reserve_predicate(reg, mask); break;
        case ScoreCommand::retire_predicate: state_.retire_predicate(reg, mask); break;
        }
    }
    void tick() override { ++cycle_; }
    uint64_t cycle() const override { return cycle_; }
    std::string_view name() const override { return "model"; }
private:
    Scoreboard state_;
    uint64_t cycle_ = 0;
};
}

bool scoreboard_rtl_available() {
#ifdef SIMT_HAS_SCOREBOARD_RTL
    return true;
#else
    return false;
#endif
}

std::unique_ptr<ScoreboardEngine> make_scoreboard_engine(bool rtl) {
    if (!rtl) return std::make_unique<ModelScoreboard>();
#ifdef SIMT_HAS_SCOREBOARD_RTL
    return make_rtl_scoreboard_engine();
#else
    throw std::runtime_error("scoreboard_rtl_backend_unavailable; build with SIMT_SCOREBOARD_RTL=ON");
#endif
}
}

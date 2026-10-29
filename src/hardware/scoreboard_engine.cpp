#include "simt/scoreboard_engine.hpp"
#include "simt/hardware_pipeline.hpp"
#include <stdexcept>

namespace simt::hw {
namespace {
class ModelScoreboard final : public ScoreboardEngine {
public:
    bool query(ScoreQuery kind, uint32_t reg, uint32_t mask) const override {
        if (kind == ScoreQuery::readable) return state_.readable(reg, mask);
        if (kind == ScoreQuery::writable) return state_.writable(reg, mask);
        return state_.predicate_ready(reg, mask);
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

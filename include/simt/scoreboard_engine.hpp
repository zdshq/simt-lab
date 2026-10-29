#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

namespace simt::hw {
enum class ScoreQuery { readable, writable, predicate };
enum class ScoreCommand { reserve_read, collect_read, reserve_write, retire_write, reserve_predicate, retire_predicate };

// Queries expose the same-cycle forwarding network; only tick commits the bounded event inputs.
// 查询暴露同拍旁路网络，只有 tick 才提交有界事件输入。
class ScoreboardEngine {
public:
    virtual ~ScoreboardEngine() = default;
    virtual bool query(ScoreQuery kind, uint32_t reg, uint32_t mask) const = 0;
    virtual void update(ScoreCommand command, uint32_t reg, uint32_t mask) = 0;
    virtual void tick() = 0;
    virtual uint64_t cycle() const = 0;
    virtual uint64_t active_cycles() const { return cycle(); }
    virtual std::string_view name() const = 0;
    bool readable(uint32_t reg, uint32_t mask) const { return query(ScoreQuery::readable, reg, mask); }
    bool writable(uint32_t reg, uint32_t mask) const { return query(ScoreQuery::writable, reg, mask); }
    bool predicate_ready(uint32_t reg, uint32_t mask) const { return query(ScoreQuery::predicate, reg, mask); }
    void reserve_read(uint32_t reg, uint32_t mask) { update(ScoreCommand::reserve_read, reg, mask); }
    void collect_read(uint32_t reg, uint32_t mask) { update(ScoreCommand::collect_read, reg, mask); }
    void reserve_write(uint32_t reg, uint32_t mask) { update(ScoreCommand::reserve_write, reg, mask); }
    void retire_write(uint32_t reg, uint32_t mask) { update(ScoreCommand::retire_write, reg, mask); }
    void reserve_predicate(uint32_t reg, uint32_t mask) { update(ScoreCommand::reserve_predicate, reg, mask); }
    void retire_predicate(uint32_t reg, uint32_t mask) { update(ScoreCommand::retire_predicate, reg, mask); }
};
bool scoreboard_rtl_available();
std::unique_ptr<ScoreboardEngine> make_scoreboard_engine(bool rtl);
std::unique_ptr<ScoreboardEngine> make_rtl_scoreboard_engine();
}

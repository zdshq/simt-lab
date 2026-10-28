#pragma once
#include "simt/module.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace simt {
constexpr uint32_t lanes = 32;
constexpr uint32_t registers = 64;
constexpr uint32_t zero_register = 63;

enum class LaneState { active, exited };
enum class Policy { round_robin, min_pc, max_pc };
struct Config {
    uint32_t sms = 2;
    uint32_t blocks = 1;
    uint32_t threads = 32;
    uint64_t max_steps = 1000000;
    Policy policy = Policy::round_robin;
};
// Store logical lane state independently from the host's CPU threads.
// 将逻辑线程状态与宿主 CPU 线程分开保存。
struct Warp {
    std::array<std::array<uint32_t, lanes>, registers> r{};
    std::array<uint32_t, 8> predicates{};
    std::array<uint32_t, lanes> pc{};
    std::array<LaneState, lanes> state{};
    uint32_t valid_mask = 0;
    std::optional<uint32_t> last_pc;
};
// A CTA owns its warps and records its assigned simulated SM.
// CTA 持有自己的 warp，并记录分配到的模拟 SM。
struct Cta {
    uint32_t block_id = 0;
    uint32_t sm_id = 0;
    uint32_t warp_cursor = 0;
    std::vector<Warp> warps;
    bool completed = false;
};
struct Group { uint32_t pc; uint32_t mask; };

class Machine {
public:
    Machine(Module module, Config config);
    void run();
    void dump(std::ostream& out, bool full_state) const;
private:
    Module module_;
    Config config_;
    std::vector<Cta> ctas_;
    std::vector<std::optional<uint32_t>> resident_;
    uint32_t next_block_ = 0;
    uint32_t sm_cursor_ = 0;
    uint64_t steps_ = 0;
    uint64_t lane_instructions_ = 0;
    std::array<uint64_t, 256> op_counts_{};
    std::optional<Group> select_group(const Warp& warp) const;
    void admit();
    bool step(Cta& cta);
    bool schedule_one(uint32_t& completed);
    void execute(Cta& cta, uint32_t warp_id, Group group);
};
}

#pragma once
#include "simt/module.hpp"
#include "simt/memory.hpp"
#include "simt/hardware_pipeline.hpp"
#include "simt/hardware_tensor.hpp"
#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace simt {
constexpr uint32_t lanes = 32;
constexpr uint32_t registers = 64;
constexpr uint32_t zero_register = 63;

enum class LaneState { active, reconvergence, yielded, cta_barrier, async_barrier, exited };
enum class Policy { round_robin, min_pc, max_pc };
enum class TokenState { unarmed, armed, dissolved };
// Reconvergence tracks a dynamic participant set separately from CTA barriers.
// 重聚跟踪动态参与集合，与 CTA 屏障分别管理。
struct Reconvergence {
    TokenState state = TokenState::unarmed;
    uint32_t participants = 0;
    uint32_t arrived = 0;
    uint32_t target_pc = 0;
};
struct Config {
    uint32_t sms = 2;
    uint32_t blocks = 1;
    uint32_t threads = 32;
    uint64_t max_steps = 1000000;
    Policy policy = Policy::round_robin;
    Policy warp_policy = Policy::round_robin;
    bool timing = false, hardware_trace = false;
    uint64_t max_cycles = 100000000;
    uint32_t cta_shared_bytes = 65536;
    hw::PipelineConfig pipeline;
    hw::TensorConfig tensor;
};
// Store logical lane state independently from the host's CPU threads.
// 将逻辑线程状态与宿主 CPU 线程分开保存。
struct Warp {
    std::array<std::array<uint32_t, lanes>, registers> r{};
    std::array<uint32_t, 8> predicates{};
    std::array<uint32_t, lanes> pc{};
    std::array<LaneState, lanes> state{};
    std::array<uint32_t, lanes> blocked_token{};
    std::array<uint32_t, lanes> waiting_phase{};
    std::array<Reconvergence, 16> reconvergence{};
    uint32_t valid_mask = 0;
    std::optional<uint32_t> last_pc;
};
struct CtaBarrier {
    bool active = false;
    uint32_t count = 0;
    uint64_t generation = 0;
    std::bitset<1024> arrived;
};
struct TransactionBarrier {
    uint32_t phase = 0;
    uint32_t expected = 0;
    uint32_t arrivals = 0;
    uint32_t bytes = 0;
    uint32_t issued = 0;
};
struct AsyncCopy {
    uint32_t destination, barrier, phase;
    uint32_t opportunities = 3;
    std::vector<uint8_t> bytes;
};
// A CTA owns its warps and records its assigned simulated SM.
// CTA 持有自己的 warp，并记录分配到的模拟 SM。
struct Cta {
    uint32_t block_id = 0;
    uint32_t sm_id = 0;
    uint32_t warp_cursor = 0;
    std::vector<Warp> warps;
    Memory shared{65536};
    std::vector<Memory> local;
    std::array<CtaBarrier, 16> barriers{};
    Memory tmem{128 * 256 * 4};
    std::bitset<256> tensor_columns;
    std::map<uint32_t, uint32_t> tensor_allocations;
    std::map<uint32_t, TransactionBarrier> transactions;
    std::vector<AsyncCopy> pending_copies;
    bool completed = false;
};
struct Group { uint32_t pc; uint32_t mask; };
class TimingMachine;

class Machine {
public:
    Machine(Module module, Config config, std::shared_ptr<Memory> global = {});
    ~Machine();
    void run();
    bool advance();
    void set_arguments(const std::vector<uint32_t>& arguments);
    uint64_t instruction_steps() const { return steps_; }
    void dump(std::ostream& out, bool full_state, bool memory = false) const;
    void load_images(const std::string& global, const std::string& constant);
private:
    friend class TimingMachine;
    Module module_;
    Config config_;
    std::shared_ptr<Memory> global_;
    Memory constant_{65536};
    std::vector<Cta> ctas_;
    std::vector<std::optional<uint32_t>> resident_;
    uint32_t next_block_ = 0;
    uint32_t completed_ = 0;
    uint32_t sm_cursor_ = 0;
    uint64_t steps_ = 0;
    uint64_t lane_instructions_ = 0;
    std::array<uint64_t, 256> op_counts_{};
    std::unique_ptr<TimingMachine> timing_;
    std::optional<Group> select_group(const Warp& warp) const;
    void admit();
    void initialize_cta(uint32_t block, uint32_t sm);
    bool step(Cta& cta);
    bool schedule_one();
    uint32_t retire_completed();
    void execute(Cta& cta, uint32_t warp_id, Group group);
    std::string deadlock_message() const;
};
}

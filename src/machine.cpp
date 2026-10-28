#include "simt/machine.hpp"
#include "simt/execution.hpp"
#include "simt/control.hpp"
#include "simt/memsync.hpp"
#include <bit>
#include <cfenv>
#include <limits>
#include <map>
#include <stdexcept>

namespace simt {
namespace {
// Validate functional limits before allocating launch state.
// 在分配启动状态之前检查功能模型的资源范围。
void validate_config(const Config& config) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    if (std::fesetround(FE_TONEAREST) != 0)
        throw std::runtime_error("unsupported_rounding_environment");
    if (config.sms == 0 || config.sms > 64 || config.blocks == 0 || config.blocks > 4096 ||
        config.threads == 0 || config.threads > 1024 || config.max_steps == 0)
        throw std::runtime_error("invalid_launch_config");
}

// Mask unused lanes in a partially filled final warp.
// 屏蔽最后一个不满 warp 中的无效线程。
Warp make_warp(uint32_t warp_id, uint32_t threads) {
    Warp warp;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        const bool valid = warp_id * lanes + lane < threads;
        warp.state[lane] = valid ? LaneState::active : LaneState::exited;
        if (valid) warp.valid_mask |= lane_bit(lane);
    }
    warp.predicates[7] = warp.valid_mask;
    return warp;
}

// Release a CTA only after all its valid lanes have retired.
// 只有全部有效线程退出后才释放 CTA。
bool retired(const Cta& cta) {
    for (const auto& warp : cta.warps)
        for (auto state : warp.state)
            if (state != LaneState::exited) return false;
    return true;
}

// Attach the virtual machine location to execution failures.
// 为执行错误附加虚拟机器中的具体位置。
std::string location(const Cta& cta, uint32_t warp, uint32_t pc) {
    return " block=" + std::to_string(cta.block_id) + " sm=" + std::to_string(cta.sm_id) +
           " warp=" + std::to_string(warp) + " pc=" + std::to_string(pc);
}
}

// Retain completed CTA state for architectural inspection.
// 保留已完成 CTA 的状态，供架构结果检查使用。
Machine::Machine(Module module, Config config) : module_(std::move(module)), config_(config) {
    validate_config(config_);
    resident_.resize(config_.sms);
    ctas_.resize(config_.blocks);
}

// Dispatch blocks into free SM slots, with one resident CTA per SM.
// 向空闲 SM 派发线程块，每个 SM 驻留一个 CTA。
void Machine::admit() {
    for (uint32_t sm = 0; sm < config_.sms && next_block_ < config_.blocks; ++sm) {
        if (resident_[sm]) continue;
        const uint32_t id = next_block_++;
        auto& cta = ctas_[id];
        cta.block_id = id;
        cta.sm_id = sm;
        cta.local.assign(config_.threads, Memory{16384});
        for (uint32_t wi = 0; wi < (config_.threads + lanes - 1) / lanes; ++wi)
            cta.warps.push_back(make_warp(wi, config_.threads));
        resident_[sm] = id;
    }
}

// Choose a PC group without assigning hardware latency to an instruction.
// 选择 PC 线程组，不为指令指定硬件延迟。
std::optional<Group> Machine::select_group(const Warp& warp) const {
    std::map<uint32_t, uint32_t> groups;
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (warp.state[lane] == LaneState::active) groups[warp.pc[lane]] |= lane_bit(lane);
    if (groups.empty()) return {};
    auto selected = groups.begin();
    if (config_.policy == Policy::max_pc) selected = std::prev(groups.end());
    else if (config_.policy == Policy::round_robin && warp.last_pc) {
        selected = groups.upper_bound(*warp.last_pc);
        if (selected == groups.end()) selected = groups.begin();
    }
    return Group{selected->first, selected->second};
}

// Execute one group before offering another warp a scheduling opportunity.
// 执行一个线程组后，将调度机会交给其他 warp。
bool Machine::step(Cta& cta) {
    const uint32_t count = static_cast<uint32_t>(cta.warps.size());
    for (uint32_t offset = 0; offset < count; ++offset) {
        uint32_t wi = (cta.warp_cursor + offset) % count;
        if (config_.warp_policy == Policy::min_pc) wi = offset;
        if (config_.warp_policy == Policy::max_pc) wi = count - 1 - offset;
        auto group = select_group(cta.warps[wi]);
        if (!group && resume_yielded(cta.warps[wi])) group = select_group(cta.warps[wi]);
        if (!group) continue;
        resume_yielded(cta.warps[wi]);
        if (steps_ >= config_.max_steps) throw std::runtime_error("step_limit_exceeded");
        try { execute(cta, wi, *group); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) + location(cta, wi, group->pc));
        }
        cta.warps[wi].last_pc = group->pc;
        cta.warp_cursor = (wi + 1) % count;
        ++steps_;
        return true;
    }
    return false;
}

// Rotate between SMs and reclaim completed block slots.
// 在 SM 之间轮转，并回收已完成线程块的位置。
bool Machine::schedule_one(uint32_t& completed) {
    for (uint32_t offset = 0; offset < config_.sms; ++offset) {
        const uint32_t sm = (sm_cursor_ + offset) % config_.sms;
        if (!resident_[sm]) continue;
        auto& cta = ctas_[*resident_[sm]];
        if (!step(cta)) continue;
        sm_cursor_ = (sm + 1) % config_.sms;
        if (retired(cta)) {
            cta.completed = true;
            resident_[sm].reset();
            ++completed;
        }
        return true;
    }
    return false;
}

// Advance the functional machine until completion or a proven lack of progress.
// 推进功能模型，直到任务完成或确认无法继续前进。
void Machine::run() {
    uint32_t completed = 0;
    while (completed < config_.blocks) {
        admit();
        if (!schedule_one(completed)) throw std::runtime_error(deadlock_message());
    }
}

// Read old values and commit all lane results together.
// 读取旧值，并统一提交所有线程的计算结果。
void Machine::execute(Cta& cta, uint32_t warp_id, Group group) {
    if (group.pc >= module_.code.size()) throw std::runtime_error("pc_out_of_range");
    auto& warp = cta.warps[warp_id];
    const auto& instruction = module_.code[group.pc];
    const auto mask = participating_mask(warp, instruction, group);
    ++op_counts_[static_cast<uint8_t>(instruction.op)];
    lane_instructions_ += std::popcount(mask);
    auto next = warp;
    const ExecutionContext context{instruction, cta, warp, next, config_, warp_id, global_, constant_};
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(group.mask & lane_bit(lane))) continue;
        next.pc[lane] = group.pc + 1;
    }
    if (!execute_control(context, group, mask, module_) && !execute_collective(context, group, mask) &&
        !execute_memory(context, mask) && !execute_barrier(context, mask))
        for (uint32_t lane = 0; lane < lanes; ++lane)
            if (mask & lane_bit(lane)) execute_scalar_lane(context, lane);
    warp = std::move(next);
    release_cta_barriers(cta);
}

// Initialize device-visible data before any CTA is admitted.
// 在准入任何 CTA 之前初始化设备可见数据。
void Machine::load_images(const std::string& global, const std::string& constant) {
    if (!global.empty()) global_.load(global);
    if (!constant.empty()) constant_.load(constant);
}
}

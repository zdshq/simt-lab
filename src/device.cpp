#include "simt/device.hpp"
#include "simt/tensor_engine.hpp"
#include "simt/cache_engine.hpp"
#include "simt/scoreboard_engine.hpp"
#include "simt/register_file.hpp"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace simt {
bool terminal(JobState state) {
    return state == JobState::complete || state == JobState::error || state == JobState::cancelled;
}

// Bound the software queue independently of compute and copy engine occupancy.
// 独立于计算和拷贝引擎占用情况，限制软件队列容量。
Device::Device(uint32_t sms, uint32_t capacity, bool timing, bool hardware_trace, bool tensor_rtl, bool cache_rtl, bool scoreboard_rtl, bool register_rtl, bool collector_rtl)
    : sms_(sms), hardware_trace_(hardware_trace), tensor_rtl_(tensor_rtl), cache_rtl_(cache_rtl),
      scoreboard_rtl_(scoreboard_rtl), register_rtl_(register_rtl), collector_rtl_(collector_rtl), ring_(capacity) {
    if (!sms || sms > 64 || !capacity || capacity > 4096) throw std::runtime_error("invalid_device_config");
    if (tensor_rtl && !timing) throw std::runtime_error("tensor_rtl_requires_timing");
    if (tensor_rtl && !hw::tensor_rtl_available()) throw std::runtime_error("tensor_rtl_backend_unavailable");
    if (cache_rtl && !timing) throw std::runtime_error("cache_rtl_requires_timing");
    if (cache_rtl && !hw::cache_rtl_available()) throw std::runtime_error("cache_rtl_backend_unavailable");
    if (scoreboard_rtl && !timing) throw std::runtime_error("scoreboard_rtl_requires_timing");
    if (scoreboard_rtl && !hw::scoreboard_rtl_available()) throw std::runtime_error("scoreboard_rtl_backend_unavailable");
    if (collector_rtl && !register_rtl) throw std::runtime_error("collector_rtl_requires_register_rtl");
    if (register_rtl && !timing) throw std::runtime_error("register_rtl_requires_timing");
    if (register_rtl && !hw::register_rtl_available()) throw std::runtime_error("register_rtl_backend_unavailable");
    if (timing) {
        hw::HierarchyConfig config; config.sms = sms;
        config.cache_rtl = cache_rtl;
        hardware_ = std::make_shared<hw::HardwareContext>(*arena.memory, config);
        arena.before_clear = [context = hardware_](uint64_t address, uint64_t size) {
            context->memory.invalidate_range(address, size);
        };
    }
    if (collector_rtl) cp_ = hw::make_command_processor(capacity);
    if (std::getenv("SIMT_MONITOR_DIR")) monitor_ = std::make_unique<MonitorPublisher>(monitor_directory().string());
    publish_monitor();
}

// Modules are decoded once and retained for repeated kernel or graph launches.
// 模块只解码一次，保留供重复 kernel 或图启动使用。
uint64_t Device::load_module(const std::vector<uint8_t>& bytes) {
    auto module = Module::from_bytes(bytes);
    const auto id = next_module_++;
    modules_.emplace(id, std::move(module));
    return id;
}

// Inspect all command operands before enqueueing or pinning resources.
// 入队和保留资源之前检查全部命令操作数。
void Device::validate(const Command& c) const {
    if (c.kind == CommandKind::kernel) {
        if (!modules_.contains(c.module)) throw std::runtime_error("invalid_module");
        if (!c.launch.blocks || c.launch.blocks > 4096 || !c.launch.threads || c.launch.threads > 1024 ||
            !c.launch.max_steps || c.arguments.size() > 16384) throw std::runtime_error("invalid_launch_config");
    }
    if (c.kind == CommandKind::graph && !graphs_.contains(c.graph)) throw std::runtime_error("invalid_graph");
    if (c.kind == CommandKind::h2d && c.size != c.data.size()) throw std::runtime_error("copy_payload_size_mismatch");
    if (c.kind == CommandKind::fill && c.value > 255) throw std::runtime_error("invalid_fill_byte");
    references(c);
}

// Graph launches retain the union of resources referenced by their nodes.
// 图启动保留全部节点所引用资源的并集。
std::set<uint32_t> Device::references(const Command& c) const {
    std::set<uint32_t> refs(c.buffers.begin(), c.buffers.end());
    for (auto base : refs) arena.validate(base);
    if (c.kind == CommandKind::h2d || c.kind == CommandKind::d2d || c.kind == CommandKind::fill)
        refs.insert(arena.containing(c.destination, c.size));
    if (c.kind == CommandKind::d2h || c.kind == CommandKind::d2d)
        refs.insert(arena.containing(c.source, c.size));
    if (c.kind == CommandKind::graph) {
        for (const auto& node : graphs_.at(c.graph).nodes) {
            const auto nested = references(node.command);
            refs.insert(nested.begin(), nested.end());
        }
    }
    return refs;
}

// Stream ordering and explicit waits become dependencies on completion, never dequeue.
// stream 顺序和显式等待依赖任务完成，而不是依赖命令出队。
JobId Device::submit(uint32_t stream, Command command, std::vector<JobId> dependencies) {
    if (written_ - fetched_count() == ring_.size()) throw std::runtime_error("submission_queue_full");
    validate(command);
    for (auto dependency : dependencies) query(dependency);
    if (stream_tail_.contains(stream)) dependencies.push_back(stream_tail_.at(stream));
    if (command.kind == CommandKind::graph && graphs_.at(command.graph).last_launch)
        dependencies.push_back(graphs_.at(command.graph).last_launch);
    Job job;
    job.id = next_job_++; job.stream = stream; job.dependencies = std::move(dependencies);
    job.pinned = references(command); job.command = std::move(command);
    arena.pin(job.pinned);
    if (job.command.kind == CommandKind::graph) {
        auto& graph = graphs_.at(job.command.graph);
        ++graph.in_flight; graph.last_launch = job.id;
    }
    const auto id = job.id;
    jobs_.emplace(id, std::move(job));
    ring_[written_++ % ring_.size()] = id;
    stream_tail_[stream] = id;
    trace(jobs_.at(id), "submit");
    return id;
}

// Publishing a write pointer makes preceding queue entries visible to the CP.
// 发布写指针，使此前的队列项对 CP 可见。
void Device::doorbell() {
    if (cp_) cp_->ring_doorbell(written_);
    else published_ = written_;
}

// The timed CP has sixteen decoded packet slots, retained through public completion.
// 时序 CP 有十六个已解码包槽位，保留至公开任务完成。
void Device::fetch() {
    if (cp_) { fetch_circuit(); return; }
    if (fetched_ == published_) return;
    if (hardware_ && fetched_ - notifications_ >= 16) return;
    auto& job = jobs_.at(ring_[fetched_++ % ring_.size()]);
    job.state = JobState::waiting;
    trace(job, "fetch");
}

// Unknown completion handles are rejected without changing device state.
// 拒绝未知完成句柄，不改变设备状态。
const Job& Device::query(JobId id) const {
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) throw std::runtime_error("invalid_job");
    return found->second;
}

// Completion publishes errors and releases ownership only after work has stopped.
// 工作停止后才发布完成或错误，并释放资源归属。
void Device::finish(Job& job, JobState state, std::string error) {
    job.state = state; job.error = std::move(error);
    job.finished = ticks_; job.recovering = false;
    if (job.machine) {
        job.instruction_steps = job.machine->instruction_steps();
        job.hardware = job.machine->hardware_json();
    }
    job.machine.reset();
    arena.unpin(job.pinned); job.pinned.clear();
    if (job.command.kind == CommandKind::graph) --graphs_.at(job.command.graph).in_flight;
    if (!job.parent) {
        if (cp_) cp_->complete(job.cp_slot, state != JobState::complete);
        else ++notifications_;
    }
    trace(job, state_name(state));
}

// Waiting is bounded and preserves pending work when the caller's limit expires.
// 等待有上限，调用方限制到期时保留待执行工作。
void Device::wait(JobId id, uint64_t limit) {
    query(id);
    while (!terminal(query(id).state) && limit--) tick();
    const auto& job = query(id);
    if (!terminal(job.state)) throw std::runtime_error("wait_timeout");
    if (job.state != JobState::complete) throw std::runtime_error(job.error);
}

// A device-wide wait drains all submissions, including independent streams after errors.
// 设备级等待排空全部提交，包括错误后仍独立的 stream。
void Device::synchronize(uint64_t limit) {
    while (notified_count() != written_ && limit--) tick();
    if (notified_count() != written_) throw std::runtime_error("wait_timeout");
    for (const auto& [id, job] : jobs_) {
        static_cast<void>(id);
        if (!job.parent && job.state != JobState::complete) throw std::runtime_error(job.error);
    }
}
}

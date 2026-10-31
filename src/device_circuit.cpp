#include "simt/device.hpp"
#include <stdexcept>

namespace simt {
namespace {
hw::EngineKind engine_kind(const Command& command) {
    if (command.kind == CommandKind::kernel) return hw::EngineKind::compute;
    if (command.kind == CommandKind::graph || command.kind == CommandKind::empty) return hw::EngineKind::none;
    return hw::EngineKind::copy;
}

uint32_t circuit_handle(JobId id) {
    if (id > UINT32_MAX) throw std::runtime_error("command_processor_handle_overflow");
    return static_cast<uint32_t>(id);
}
}

uint64_t Device::fetched_count() const { return cp_ ? cp_->fetched() : fetched_; }
uint64_t Device::published_count() const { return cp_ ? cp_->published() : published_; }
uint64_t Device::notified_count() const { return cp_ ? cp_->notified() : notifications_; }

// Engine occupancy is released in the circuit; the host reference only names the running job.
// 引擎占用在电路中释放；宿主引用只标明正在运行的任务。
void Device::release_compute() {
    compute_.reset();
    if (cp_) cp_->release(hw::EngineKind::compute);
}

void Device::release_copy() {
    copy_.reset();
    if (cp_) cp_->release(hw::EngineKind::copy);
}

// Dependencies still in a decoded slot become wait bits; older ones are read as completion records.
// 仍在解码槽中的依赖转换为等待位；更早的依赖按完成记录读取。
void Device::fetch_circuit() {
    const auto slot = cp_->fetch_slot();
    if (!slot) return;
    auto& job = jobs_.at(ring_[cp_->fetched() % ring_.size()]);
    uint32_t wait = 0;
    job.external.clear();
    for (auto dependency : job.dependencies) {
        const auto match = cp_->lookup(circuit_handle(dependency));
        if (match) wait |= match;
        else job.external.push_back(dependency);
    }
    cp_->accept_fetch(circuit_handle(job.id), engine_kind(job.command), wait);
    job.state = JobState::waiting; job.cp_slot = *slot;
    cp_jobs_[*slot] = job.id;
    trace(job, "fetch");
}

void Device::offer_external_dependencies() {
    uint32_t pending = 0, failed = 0;
    for (const auto id : cp_jobs_) {
        const auto found = jobs_.find(id);
        if (found == jobs_.end() || found->second.state != JobState::waiting) continue;
        const auto& job = found->second;
        for (const auto dependency : job.external) {
            const auto state = query(dependency).state;
            if (!terminal(state)) pending |= uint32_t{1} << job.cp_slot;
            else if (state != JobState::complete) failed |= uint32_t{1} << job.cp_slot;
        }
    }
    cp_->offer_external(pending, failed);
}

// Graph node readiness remains host logic; the circuit still arbitrates the winner against packets.
// 图节点就绪判断仍是宿主逻辑；电路仍负责它与队列包之间的仲裁。
std::optional<hw::CpNode> Device::oldest_node() const {
    for (const auto& [id, job] : jobs_) {
        if (!job.parent || job.state != JobState::waiting) continue;
        const auto ready = readiness(job);
        if (!ready) continue;
        const auto kind = engine_kind(job.command);
        if (ready < 0) return hw::CpNode{circuit_handle(id), kind, true};
        if (kind == hw::EngineKind::none || !cp_->busy(kind)) return hw::CpNode{circuit_handle(id), kind, false};
    }
    return {};
}

// The circuit chooses one candidate each clock; start failures return its claimed engine.
// 电路每拍选择一个候选；启动失败时归还已声明的引擎。
void Device::dispatch_circuit() {
    offer_external_dependencies();
    const auto node = oldest_node();
    cp_->offer_node(node);
    const auto decision = cp_->dispatch();
    if (!decision) return;
    auto& job = jobs_.at(decision->node ? JobId{node->handle} : cp_jobs_[decision->slot]);
    if (decision->cancel) { finish(job, JobState::cancelled, "dependency_failed"); return; }
    try { start(job); }
    catch (const std::exception& error) {
        const auto kind = engine_kind(job.command);
        if (kind != hw::EngineKind::none) cp_->release(kind);
        finish(job, JobState::error, error.what());
    }
}

// One shared decoder serves the oldest running graph that still has unexpanded nodes.
// 一个共享解码器服务仍有未展开节点的最早运行图。
void Device::expand_circuit_graph() {
    uint32_t pending = 0;
    for (const auto id : cp_jobs_) {
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) continue;
        const auto& job = found->second;
        if (job.command.kind == CommandKind::graph && job.state == JobState::running && job.expanded != job.children.size())
            pending |= uint32_t{1} << job.cp_slot;
    }
    if (const auto slot = cp_->select_graph(pending)) expand_node(jobs_.at(cp_jobs_[*slot]));
}
}

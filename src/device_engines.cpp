#include "simt/device.hpp"
#include <algorithm>

namespace simt {
// A failed predecessor cancels descendants, while independent branches remain runnable.
// 前驱失败会取消后继，独立分支仍可运行。
int Device::readiness(const Job& job) const {
    bool ready = true;
    for (auto id : job.dependencies) {
        const auto state = query(id).state;
        if (state == JobState::error || state == JobState::cancelled) return -1;
        ready &= state == JobState::complete;
    }
    return ready ? 1 : 0;
}

// Reserve an engine only after a dependency-ready command has been selected.
// 只有选择了依赖已满足的命令之后才占用引擎。
void Device::start(Job& job) {
    job.state = JobState::running;
    trace(job, "start");
    const auto& c = job.command;
    if (c.kind == CommandKind::graph) { expand_graph(job); return; }
    if (c.kind == CommandKind::empty) { finish(job); return; }
    if (c.kind == CommandKind::kernel) {
        auto config = c.launch; config.sms = sms_;
        job.machine = std::make_unique<Machine>(modules_.at(c.module), config, arena.memory);
        job.machine->set_arguments(c.arguments);
        compute_ = job.id;
        return;
    }
    if (c.kind == CommandKind::d2h || c.kind == CommandKind::d2d) job.result.resize(c.size);
    if (c.kind == CommandKind::d2d)
        for (uint32_t offset = 0; offset < c.size; ++offset)
            job.result[offset] = static_cast<uint8_t>(arena.memory->read(uint64_t{c.source} + offset, 1));
    copy_ = job.id;
}

// Oldest ready commands claim free engines without blocking other streams on dependencies.
// 最早就绪命令取得空闲引擎，依赖等待不阻塞其他 stream。
void Device::dispatch() {
    for (auto& [id, job] : jobs_) {
        static_cast<void>(id);
        if (job.state != JobState::waiting) continue;
        const auto ready = readiness(job);
        if (ready < 0) { finish(job, JobState::cancelled, "dependency_failed"); continue; }
        if (!ready) continue;
        const auto kind = job.command.kind;
        if (kind == CommandKind::kernel && compute_) continue;
        if (kind != CommandKind::kernel && kind != CommandKind::graph && kind != CommandKind::empty && copy_) continue;
        try { start(job); }
        catch (const std::exception& error) { finish(job, JobState::error, error.what()); }
    }
}

// Each compute service executes at most one SIMT instruction group.
// 每次计算服务最多执行一个 SIMT 指令组。
void Device::service_compute() {
    if (!compute_) return;
    auto& job = jobs_.at(*compute_);
    try {
        if (!job.machine->advance()) return;
        compute_.reset();
        finish(job);
    } catch (const std::exception& error) {
        compute_.reset();
        finish(job, JobState::error, error.what());
    }
}

// Copies advance by bounded byte chunks independently of the compute engine.
// 拷贝按有界字节块推进，独立于计算引擎。
void Device::service_copy() {
    if (!copy_) return;
    auto& job = jobs_.at(*copy_);
    const auto& c = job.command;
    const auto end = job.offset + std::min(uint32_t{64}, c.size - job.offset);
    for (; job.offset < end; ++job.offset) {
        const auto offset = job.offset;
        if (c.kind == CommandKind::d2h)
            job.result[offset] = static_cast<uint8_t>(arena.memory->read(uint64_t{c.source} + offset, 1));
        else {
            const auto value = c.kind == CommandKind::h2d ? c.data[offset] :
                               c.kind == CommandKind::d2d ? job.result[offset] : c.value;
            arena.memory->write(uint64_t{c.destination} + offset, 1, value);
        }
    }
    if (job.offset == c.size) { copy_.reset(); finish(job); }
}

// Ticks are host-driven service opportunities, not a hardware clock or latency model.
// tick 是主机驱动的服务机会，不是硬件时钟或延迟模型。
void Device::tick() {
    ++ticks_;
    fetch();
    dispatch();
    service_copy();
    service_compute();
    finish_graphs();
}
}

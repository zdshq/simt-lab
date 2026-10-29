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
    job.started = ticks_;
    trace(job, "start");
    const auto& c = job.command;
    if (c.kind == CommandKind::graph) { expand_graph(job); return; }
    if (c.kind == CommandKind::empty) { finish(job); return; }
    if (c.kind == CommandKind::kernel) { start_kernel(job); return; }
    start_copy(job);
}

// All timed kernels share device caches and receive the device's single external clock.
// 所有时序 kernel 共享设备缓存，并接收设备唯一的外部时钟。
void Device::start_kernel(Job& job) {
    const auto& c = job.command;
    auto config = c.launch; config.sms = sms_;
    config.timing = bool(hardware_); config.hardware_trace = hardware_trace_;
    job.machine = std::make_unique<Machine>(modules_.at(c.module), config, arena.memory, hardware_);
    job.machine->set_arguments(c.arguments);
    compute_ = job.id;
}

// Functional D2D retains its snapshot; timed D2D uses bounded directional chunks instead.
// 功能 D2D 保留源快照；时序 D2D 使用有界的定向数据块。
void Device::start_copy(Job& job) {
    const auto& c = job.command;
    if (c.kind == CommandKind::d2h || (!hardware_ && c.kind == CommandKind::d2d)) job.result.resize(c.size);
    if (!hardware_ && c.kind == CommandKind::d2d)
        for (uint32_t offset = 0; offset < c.size; ++offset)
            job.result[offset] = static_cast<uint8_t>(arena.memory->read(uint64_t{c.source} + offset, 1));
    transfer_ = {};
    if (hardware_) transfer_.owner = hardware_->new_owner();
    copy_ = job.id;
}

// Oldest ready commands claim free engines without blocking other streams on dependencies.
// 最早就绪命令取得空闲引擎，依赖等待不阻塞其他 stream。
void Device::dispatch() {
    for (auto& [id, job] : jobs_) {
        static_cast<void>(id);
        if (job.state != JobState::waiting) continue;
        const auto ready = readiness(job);
        if (ready < 0) {
            finish(job, JobState::cancelled, "dependency_failed");
            if (hardware_) return;
            continue;
        }
        if (!ready) continue;
        const auto kind = job.command.kind;
        if (kind == CommandKind::kernel && compute_) continue;
        if (kind != CommandKind::kernel && kind != CommandKind::graph && kind != CommandKind::empty && copy_) continue;
        try { start(job); }
        catch (const std::exception& error) { finish(job, JobState::error, error.what()); }
        if (hardware_) return;
    }
}

// Faulted timed kernels stop issue but retain storage and pins until accepted requests drain.
// 出错的时序 kernel 停止发射，但保留存储和引用直至已接受请求排空。
void Device::service_compute() {
    if (!compute_) return;
    auto& job = jobs_.at(*compute_);
    if (job.recovering) {
        if (!job.machine->drain()) return;
        compute_.reset();
        finish(job, JobState::error, job.error);
        return;
    }
    try {
        if (!job.machine->advance()) return;
        compute_.reset();
        finish(job);
    } catch (const std::exception& error) {
        if (hardware_) {
            job.error = error.what(); job.recovering = true;
            trace(job, "fault_drain");
            return;
        }
        compute_.reset();
        finish(job, JobState::error, error.what());
    }
}

// Copies advance by bounded byte chunks independently of the compute engine.
// 拷贝按有界字节块推进，独立于计算引擎。
void Device::service_copy() {
    if (!copy_) return;
    auto& job = jobs_.at(*copy_);
    if (hardware_) { service_timed_copy(job); return; }
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

// Timing mode advances one shared clock; functional mode retains service-opportunity semantics.
// 时序模式推进一次共享时钟；功能模式保持服务机会语义。
void Device::tick() {
    ++ticks_;
    fetch();
    service_graphs();
    dispatch();
    if (hardware_) hardware_->tick();
    service_copy();
    service_compute();
    finish_graphs();
    if (ticks_ % 64 == 0) publish_monitor();
}
}

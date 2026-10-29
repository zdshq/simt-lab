#include "simt/device.hpp"
#include <sstream>
#include <stdexcept>

namespace simt {
const char* state_name(JobState state) {
    switch (state) {
    case JobState::queued: return "queued";
    case JobState::waiting: return "waiting";
    case JobState::running: return "running";
    case JobState::complete: return "complete";
    case JobState::error: return "error";
    case JobState::cancelled: return "cancelled";
    }
    throw std::runtime_error("invalid_job_state");
}

// Bound trace retention explicitly and report dropped entries to callers.
// 显式限制跟踪记录数量，并向调用方报告丢弃条数。
void Device::trace(const Job& job, const std::string& action) {
    if (trace_.size() == 100000) { trace_.pop_front(); ++dropped_trace_; }
    trace_.push_back({ticks_, job.id, job.parent, job.node, action});
}

// Querying completion never advances the device or exposes an incomplete copy result.
// 查询完成状态不会推进设备，也不会暴露未完成的拷贝结果。
std::string Device::job_json(JobId id) const {
    const auto& job = query(id);
    std::ostringstream out;
    out << "{\"id\":" << id << ",\"state\":" << json_string(state_name(job.state))
        << ",\"error\":" << json_string(job.error) << ",\"instruction_steps\":" << job.instruction_steps;
    out << ",\"data\":" << json_string(job.state == JobState::complete && job.command.kind == CommandKind::d2h
                                        ? encode_hex(job.result) : "") << ",\"children\":[";
    for (size_t index = 0; index < job.children.size(); ++index) {
        if (index) out << ',';
        out << job.children[index];
    }
    out << "]}";
    return out.str();
}

// Queue progress, engine occupancy, and completion notifications have separate counters.
// 队列推进、引擎占用和完成通知分别计数。
std::string Device::stats_json() const {
    std::ostringstream out;
    out << "{\"sms\":" << sms_ << ",\"capacity\":" << ring_.size() << ",\"write\":" << written_
        << ",\"read\":" << fetched_ << ",\"doorbell\":" << published_ << ",\"ticks\":" << ticks_
        << ",\"completed\":" << notifications_ << ",\"compute\":" << compute_.value_or(0)
        << ",\"copy\":" << copy_.value_or(0) << ",\"dropped_trace\":" << dropped_trace_ << '}';
    return out.str();
}

// Internal graph nodes retain parent and node indices for dependency audits.
// 内部图节点保留父任务与节点索引，供依赖关系审查。
std::string Device::trace_json() const {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& entry : trace_) {
        if (!first) out << ',';
        first = false;
        out << "{\"tick\":" << entry.tick << ",\"job\":" << entry.job << ",\"parent\":" << entry.parent
            << ",\"node\":" << entry.node << ",\"action\":" << json_string(entry.action) << '}';
    }
    out << ']';
    return out.str();
}
}

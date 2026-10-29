#include "simt/timing.hpp"

namespace simt {
// Counters distinguish submitted commands, payload acknowledgements, and published completions.
// 计数分别区分已提交命令、载荷确认和已发布完成。
void TimingMachine::dump_async(std::ostream& out) const {
    const auto& config = machine_.config_.async;
    out << ",\"async_config\":{\"commands\":" << config.commands << ",\"fragments\":" << config.fragments
        << ",\"chunk_bytes\":" << config.chunk_bytes << ",\"barrier_slots\":" << config.barrier_slots
        << ",\"barrier_latency\":" << config.barrier_latency << "},\"async\":[";
    for (uint32_t sm = 0; sm < sms_.size(); ++sm) {
        if (sm) out << ',';
        const auto& s = sms_[sm]->async_stats;
        out << "{\"submitted\":" << s.copies << ",\"completed\":" << s.completed
            << ",\"read_bytes\":" << s.read_bytes << ",\"write_bytes\":" << s.write_bytes
            << ",\"fragments\":" << s.fragments << ",\"command_stalls\":" << s.command_stalls
            << ",\"fragment_stalls\":" << s.fragment_stalls << ",\"memory_stalls\":" << s.memory_stalls
            << ",\"peak_commands\":" << s.peak_commands << ",\"barrier_requests\":" << s.barrier_requests
            << ",\"barrier_stalls\":" << s.barrier_stalls << ",\"wait_queries\":" << s.wait_queries
            << ",\"phase_completions\":" << s.phase_completions << '}';
    }
    out << ']';
}
}

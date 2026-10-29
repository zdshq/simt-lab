#include "simt/machine.hpp"
#include "simt/timing.hpp"
#include <sstream>

namespace simt {
std::string Machine::hardware_json() const {
    if (!timing_) return "null";
    std::ostringstream out;
    timing_->dump(out);
    return out.str();
}

namespace {
// Serialize arrays in index order for stable snapshots.
// 按索引顺序序列化数组，确保快照稳定。
template<class T> void array_json(std::ostream& out, const T& values) {
    out << '[';
    bool first = true;
    for (auto value : values) {
        if (!first) out << ',';
        first = false;
        out << value;
    }
    out << ']';
}

// Report functional instruction counts, not hardware cycles.
// 报告功能指令计数，而非硬件周期。
void dump_counts(std::ostream& out, const std::array<uint64_t, 256>& counts) {
    out << '{';
    bool first = true;
    for (uint32_t op = 0; op < counts.size(); ++op) {
        if (!counts[op]) continue;
        if (!first) out << ',';
        first = false;
        out << '"' << op_name(static_cast<Op>(op)) << "\":" << counts[op];
    }
    out << '}';
}

// Preserve inactive lane slots so register indices remain stable.
// 保留无效线程位置，使寄存器索引稳定。
void dump_warp(std::ostream& out, const Warp& warp) {
    out << "{\"valid_mask\":" << warp.valid_mask << ",\"pc\":";
    array_json(out, warp.pc);
    out << ",\"predicates\":";
    array_json(out, warp.predicates);
    out << ",\"registers\":[";
    for (uint32_t reg = 0; reg < registers; ++reg) {
        if (reg) out << ',';
        array_json(out, warp.r[reg]);
    }
    out << "]}";
}

// Keep placement visible even without the detailed state dump.
// 即使不转储详细状态，也保留派发位置。
void dump_cta(std::ostream& out, const Cta& cta, bool full_state, bool memory) {
    out << "{\"block_id\":" << cta.block_id << ",\"sm_id\":" << cta.sm_id
        << ",\"completed\":" << (cta.completed ? "true" : "false");
    if (full_state) {
        out << ",\"warps\":[";
        for (size_t wi = 0; wi < cta.warps.size(); ++wi) {
            if (wi) out << ',';
            dump_warp(out, cta.warps[wi]);
        }
        out << ']';
    }
    if (memory) {
        out << ",\"shared\":";
        cta.shared.dump(out);
        out << ",\"tmem\":";
        cta.tmem.dump(out);
        out << ",\"local\":[";
        for (size_t thread = 0; thread < cta.local.size(); ++thread) {
            if (thread) out << ',';
            cta.local[thread].dump(out);
        }
        out << ']';
    }
    out << '}';
}
}

// Emit JSON consumed by the CLI and independent reference tests.
// 输出供命令行和独立参考测试读取的 JSON。
void Machine::dump(std::ostream& out, bool full_state, bool memory) const {
    out << "{\"status\":\"ok\",\"sm_count\":" << config_.sms
        << ",\"blocks_completed\":" << config_.blocks << ",\"instruction_steps\":" << steps_
        << ",\"lane_instructions\":" << lane_instructions_ << ",\"op_counts\":";
    dump_counts(out, op_counts_);
    out << ",\"ctas\":[";
    for (size_t ci = 0; ci < ctas_.size(); ++ci) {
        if (ci) out << ',';
        dump_cta(out, ctas_[ci], full_state, memory);
    }
    out << ']';
    if (memory) {
        out << ",\"global\":";
        global_->dump(out);
        out << ",\"constant\":";
        constant_.dump(out);
    }
    if (timing_) { out << ",\"hardware\":"; timing_->dump(out); }
    out << "}\n";
}
}

#include "simt/timing.hpp"
#include <ostream>

namespace simt {
namespace {
void partition_counts(std::ostream& out,const hw::InstructionFetchStats& s,uint32_t part) {
    out << "{\"partition\":" << part << ",\"queries\":" << s.queries[part] << ",\"delivered\":" << s.delivered[part]
        << ",\"stalls\":" << s.stalls[part] << ",\"reads\":" << s.reads[part] << ",\"hits\":" << s.hits[part]
        << ",\"misses\":" << s.misses[part] << ",\"fill_words\":" << s.fills[part]
        << ",\"address_errors\":" << s.address_errors[part] << '}';
}
}

// Capacity describes actual RTL arrays; refill latency belongs to the separately identified memory model.
// 容量描述实际 RTL 阵列；补充延迟属于单独标明的存储模型。
void TimingMachine::dump_instruction_fetch(std::ostream& out) const {
    out << ",\"instruction_fetch\":["; bool first=true;
    for (uint32_t sm=0; sm<sms_.size(); ++sm) {
        const auto& unit=sms_[sm]->instruction_fetch; if (!unit) continue;
        if (!first) out << ',';
        first=false; const auto& s=unit->stats();
        out << "{\"sm\":" << sm << ",\"backend\":\"rtl\",\"physical_partitions\":8,\"partition_limit\":" << sms_[sm]->partitions.size()
            << ",\"bytes_per_partition\":4096,\"line_bytes\":64,\"refill_ports\":1,\"memory_backend\":\"model\",\"memory_latency\":8"
            << ",\"clocks\":" << s.clocks << ",\"busy\":" << (unit->busy() ? "true" : "false")
            << ",\"memory_requests\":" << s.memory_requests << ",\"memory_responses\":" << s.memory_responses << ",\"partitions\":[";
        for (uint32_t part=0; part<8; ++part) { if (part) out << ','; partition_counts(out,s,part); }
        out << "]}";
    }
    out << ']';
}
}

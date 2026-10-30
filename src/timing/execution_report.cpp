#include "simt/timing.hpp"
#include <ostream>

namespace simt {
namespace {
template<class Values> void field(std::ostream& out,const char* name,const Values& values) {
    out << ",\"" << name << "\":[";
    for (uint32_t at=0; at<values.size(); ++at) { if (at) out << ','; out << values[at]; }
    out << ']';
}
void events(std::ostream& out,const hw::ExecutionArbiterStats& s) {
    out << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks << ",\"tensor_releases\":" << s.tensor_releases;
    field(out,"requests",s.requests); field(out,"grants",s.grants); field(out,"accepted",s.accepted);
    field(out,"interval_stalls",s.interval_stalls); field(out,"shared_stalls",s.shared_stalls);
    field(out,"arbitration_stalls",s.arbitration_stalls); field(out,"downstream_stalls",s.downstream_stalls);
}
}

// Snapshot ports expose retained hardware state without adding clocks or accepting candidates.
// 快照端口暴露硬件保持状态，不增加时钟或接受候选。
void TimingMachine::dump_execution_arbiters(std::ostream& out) const {
    out << ",\"execution_arbiters\":["; bool first=true;
    for (uint32_t sm=0; sm<sms_.size(); ++sm) {
        const auto& unit=sms_[sm]->execution_arbiter; if (!unit) continue;
        if (!first) out << ',';
        first=false; const auto state=unit->read();
        out << "{\"sm\":" << sm << ",\"backend\":\"rtl\",\"physical_partitions\":8,\"ports_per_partition\":7,\"storage_bits\":778"
            << ",\"limit\":" << sms_[sm]->partitions.size() << ",\"tensor_busy\":" << (state.tensor_busy ? "true" : "false")
            << ",\"tensor_owner\":" << state.tensor_owner << ",\"tensor_cursor\":" << state.tensor_cursor
            << ",\"async_cursor\":" << state.async_cursor;
        field(out,"cooldowns",state.cooldowns); events(out,unit->stats()); out << '}';
    }
    out << ']';
}
}

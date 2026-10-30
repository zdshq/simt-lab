#include "simt/timing.hpp"
#include <ostream>

namespace simt {
// Counts describe one combinational decode opportunity per partition, not clocked decoder storage.
// 计数描述每分区一次组合译码机会，不表示译码器具有时钟存储。
void TimingMachine::dump_instruction_decoders(std::ostream& out) const {
    out << ",\"instruction_decoders\":["; bool first=true;
    for (uint32_t sm=0; sm<sms_.size(); ++sm) for (uint32_t part=0; part<sms_[sm]->partitions.size(); ++part) {
        const auto& unit=sms_[sm]->partitions[part].decoder; if (!unit) continue;
        if (!first) out << ',';
        first=false; const auto& s=unit->stats();
        out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"instruction_bits\":128,\"source_ports\":3,\"storage_bits\":0"
            << ",\"cycles\":" << s.cycles << ",\"inspected\":" << s.inspected << ",\"operands\":" << s.operands
            << ",\"accepted\":" << s.accepted << ",\"sources\":" << s.sources << ",\"encoding_errors\":" << s.encoding_errors
            << ",\"operand_errors\":" << s.operand_errors << '}';
    }
    out << ']';
}
}

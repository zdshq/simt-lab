#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
static_assert(static_cast<uint32_t>(Op::FSETP) == 15 && static_cast<uint32_t>(Op::FADD) == 16 &&
    static_cast<uint32_t>(Op::FMUL) == 17 && static_cast<uint32_t>(Op::FFMA) == 18 &&
    static_cast<uint32_t>(Op::I2F) == 22 && static_cast<uint32_t>(Op::F2I) == 23);

// Issue metadata onto port one; only the RTL handshake releases the collected source vectors.
// 将发射元数据送到端口一；只有 RTL 握手才释放已收集源向量。
bool TimingMachine::launch_floating(Inflight& entry) {
    auto* unit = sms_[entry.sm]->partitions[entry.partition].registers->floating();
    if (!unit || !entry.completion) throw std::runtime_error("floating_rtl_missing_completion_path");
    const auto& i = entry.instruction;
    const hw::FloatingCommand command{*entry.collector,*entry.completion,static_cast<uint32_t>(i.op),i.mode,
        i.ra,i.rb,i.rc,entry.mask,machine_.config_.pipeline.float_latency};
    if (!unit->launch(command)) return false;
    release_collector_owner(entry); entry.stage = Stage::floating;
    return true;
}
}

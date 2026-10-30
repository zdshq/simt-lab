#include "simt/timing.hpp"
#include <stdexcept>

namespace simt {
static_assert(static_cast<uint32_t>(Op::ELECT) == 30 && static_cast<uint32_t>(Op::VOTE) == 31 &&
              static_cast<uint32_t>(Op::SHFL) == 32 && static_cast<uint32_t>(Op::ACTIVEMASK) == 33);

bool TimingMachine::launch_collective(Inflight& entry) {
    auto& sm = *sms_[entry.sm];
    auto* unit = sm.partitions[entry.partition].registers->collective();
    if (!unit || !entry.completion) throw std::runtime_error("collective_rtl_missing_completion_path");
    const auto& i = entry.instruction;
    const auto slot = warps_[entry.cta][entry.warp].slot / sm.partitions.size();
    const hw::CollectiveCommand command{*entry.collector,*entry.completion,static_cast<uint32_t>(i.op),i.mode,
        i.ra,i.rb,entry.mask,i.imm,entry.group.mask,static_cast<uint32_t>(slot)};
    if (!unit->launch(command)) return false;
    release_collector_owner(entry); entry.stage = Stage::collective; entry.collective_result = true;
    return true;
}
}

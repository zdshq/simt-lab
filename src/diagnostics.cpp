#include "simt/machine.hpp"
#include <sstream>

namespace simt {
// Report one blocked lane per warp to keep deadlock messages bounded and useful.
// 每个 warp 报告一个阻塞线程，使死锁信息保持简洁实用。
std::string Machine::deadlock_message() const {
    std::ostringstream out;
    out << "deadlock_no_runnable_group";
    for (const auto& slot : resident_) {
        if (!slot) continue;
        const auto& cta = ctas_[*slot];
        for (uint32_t wi = 0; wi < cta.warps.size(); ++wi) {
            const auto& warp = cta.warps[wi];
            for (uint32_t lane = 0; lane < lanes; ++lane) {
                if (warp.state[lane] == LaneState::exited) continue;
                out << " [block=" << cta.block_id << " sm=" << cta.sm_id << " warp=" << wi
                    << " lane=" << lane << " pc=" << warp.pc[lane] << " wait="
                    << (warp.state[lane] == LaneState::cta_barrier ? "cta_barrier" : "reconvergence")
                    << " token=" << warp.blocked_token[lane] << ']';
                break;
            }
        }
    }
    return out.str();
}
}

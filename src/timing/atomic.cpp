#include "simt/timing.hpp"
#include "simt/async.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
// Validate the entire vector before any lane enters a bank or publishes a partial atomic update.
// 任意 lane 进入 bank 或发布部分原子更新前，先验证整个向量。
void TimingMachine::prepare_atomic(Inflight& entry) {
    const auto& i = entry.instruction; auto& cta = machine_.ctas_[entry.cta];
    if (i.aux > 1) throw std::runtime_error("invalid_atomic_space");
    if (i.mode > 9) throw std::runtime_error("invalid_atomic_mode");
    hw::WarpAccess access; access.mask = entry.mask; access.write = true; access.atomic = i.mode;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(entry.mask & lane_bit(lane))) continue;
        const auto address = int64_t{read_register(entry.operands, i.ra, lane)} + std::bit_cast<int32_t>(i.imm);
        if (address < 0 || address > UINT32_MAX) throw std::runtime_error("memory_address_overflow");
        auto& storage = i.aux == 0 ? *machine_.global_ : cta.shared;
        storage.check(static_cast<uint64_t>(address), 4, 4);
        if (i.aux == 1) check_shared_write(cta, static_cast<uint64_t>(address), 4);
        access.address[lane] = static_cast<uint64_t>(address);
        access.value[lane] = read_register(entry.operands, i.rb, lane);
        access.replacement[lane] = read_register(entry.operands, i.rc, lane);
    }
    if (entry.mask) entry.memory.push_back({i.aux, access, {}, false});
}

void TimingMachine::dump_atomic(std::ostream& out) const {
    const auto& config = machine_.config_.atomic; const auto& stats = memory_->atomic_stats();
    out << ",\"atomic_config\":{\"global_banks\":" << config.banks << ",\"latency\":" << config.latency
        << ",\"capacity\":" << config.capacity << "},\"global_atomic\":{\"requests\":" << stats.requests
        << ",\"lanes\":" << stats.lanes << ",\"completed\":" << stats.completed
        << ",\"bank_stalls\":" << stats.bank_stalls << ",\"queue_stalls\":" << stats.queue_stalls
        << ",\"read_stalls\":" << stats.read_stalls << ",\"write_stalls\":" << stats.write_stalls << '}';
}
}

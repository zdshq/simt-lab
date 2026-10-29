#include "simt/timing.hpp"

namespace simt {
// All timing stages report the instruction's original issue location.
// 所有时序阶段均报告指令原始发射位置。
std::string timing_location(uint32_t cta, uint32_t sm, uint32_t warp, uint32_t pc) {
    return " block=" + std::to_string(cta) + " sm=" + std::to_string(sm) +
           " warp=" + std::to_string(warp) + " pc=" + std::to_string(pc);
}

namespace {
// Report each physical file and actual accepted port events, including partitions that remain idle.
// 报告每份物理寄存器堆与实际接受的端口事件，包括始终空闲的分区。
void register_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"register_files\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            if (!file) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = file->stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"slots\":" << file->slots()
                << ",\"port_groups\":" << file->port_groups() << ",\"bytes\":" << file->slots()*8192 << ",\"banks\":4,\"clocks\":" << s.clocks
                << ",\"active_clocks\":" << s.active_clocks << ",\"reads\":" << s.reads
                << ",\"writes\":" << s.writes << ",\"cleared_slots\":" << s.cleared_slots << '}';
        }
    out << ']';
}

void scoreboard_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"scoreboards\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t slot = 0; slot < sms[sm]->scoreboards.size(); ++slot) {
            const auto& board = sms[sm]->scoreboards[slot];
            if (!board) continue;
            if (!first) out << ',';
            first = false;
            out << "{\"sm\":" << sm << ",\"slot\":" << slot << ",\"backend\":\"" << board->name()
                << "\",\"clocks\":" << board->cycle() << ",\"active_clocks\":" << board->active_cycles() << '}';
        }
    out << ']';
}

void cache_stats(std::ostream& out, const hw::CachePort& cache) {
    const auto& s = cache.stats(); const auto& config = cache.config();
    out << "{\"backend\":\"" << cache.name() << "\",\"clocks\":" << cache.cycle()
        << ",\"bytes\":" << config.bytes << ",\"ways\":" << config.ways << ",\"ports\":" << config.banks
        << ",\"miss_entries\":" << config.miss_entries << ",\"response_capacity\":" << config.capacity
        << ",\"hits\":" << s.hits << ",\"sector_misses\":" << s.sector_misses
        << ",\"line_misses\":" << s.line_misses << ",\"merges\":" << s.merges
        << ",\"evictions\":" << s.evictions << ",\"mshr_stalls\":" << s.mshr_stalls
        << ",\"bank_stalls\":" << s.port_stalls << ",\"fill_stalls\":" << s.fill_stalls
        << ",\"fill_bytes\":" << s.fill_bytes << '}';
}
}

void TimingMachine::trace(const Inflight& entry, const std::string& action) {
    if (!machine_.config_.hardware_trace) return;
    if (trace_.size() == 100000) { trace_.pop_front(); ++stats_.dropped_trace; }
    trace_.push_back({cycle_, entry.id, entry.sm, entry.cta, entry.warp, entry.group.pc, action});
}

void TimingMachine::dump_statistics(std::ostream& out) const {
    out << "{\"model\":\"simt-research-v1\",\"cycles\":" << cycle_ - started_
        << ",\"clock_start\":" << started_ << ",\"clock_end\":" << cycle_
        << ",\"memory_counters_scope\":\"device_cumulative\""
        << ",\"issued\":" << stats_.issued << ",\"retired\":" << stats_.retired
        << ",\"scoreboard_stalls\":" << stats_.scoreboard_stalls << ",\"boundary_stalls\":" << stats_.boundary_stalls
        << ",\"collector_stalls\":" << stats_.collector_stalls << ",\"register_bank_stalls\":" << stats_.register_stalls
        << ",\"unit_stalls\":" << stats_.unit_stalls << ",\"writeback_stalls\":" << stats_.writeback_stalls
        << ",\"memory_order_stalls\":" << stats_.memory_order_stalls << ",\"peak_ctas_per_sm\":" << stats_.peak_ctas
        << ",\"dropped_trace\":" << stats_.dropped_trace;
}

void TimingMachine::dump_memory(std::ostream& out) const {
    out << ",\"l1\":[";
    for (uint32_t sm = 0; sm < sms_.size(); ++sm) {
        if (sm) out << ',';
        cache_stats(out, memory_->l1_cache(sm));
    }
    out << "],\"l2\":"; cache_stats(out, memory_->l2_cache());
    const auto& dram = memory_->dram_stats();
    out << ",\"dram\":{\"reads\":" << dram.reads << ",\"writes\":" << dram.writes
        << ",\"row_hits\":" << dram.row_hits << ",\"row_misses\":" << dram.row_misses << "},\"shared\":[";
    for (uint32_t sm = 0; sm < sms_.size(); ++sm) {
        if (sm) out << ',';
        const auto& shared = sms_[sm]->shared.stats();
        out << "{\"bank_services\":" << shared.bank_services << ",\"broadcasts\":" << shared.broadcasts
            << ",\"bank_stalls\":" << shared.bank_stalls << ",\"atomic_started\":" << shared.atomic_started
            << ",\"atomic_completed\":" << shared.atomic_completed << '}';
    }
    out << ']';
}

void TimingMachine::dump_trace(std::ostream& out) const {
    out << ",\"trace\":[";
    bool first = true;
    for (const auto& event : trace_) {
        if (!first) out << ',';
        first = false;
        out << "{\"cycle\":" << event.cycle << ",\"instruction\":" << event.instruction
            << ",\"sm\":" << event.sm << ",\"block\":" << event.cta << ",\"warp\":" << event.warp
            << ",\"pc\":" << event.pc << ",\"action\":\"" << event.action << "\"}";
    }
    out << ']';
}

void TimingMachine::dump_tensor(std::ostream& out) const {
    const auto& config = machine_.config_.tensor;
    out << ",\"tensor_config\":{\"rows\":" << config.rows << ",\"columns\":" << config.columns
        << ",\"latency\":" << config.latency << ",\"input_capacity\":" << config.input_capacity
        << ",\"tmem_banks\":" << config.tmem_banks << ",\"tmem_latency\":" << config.tmem_latency
        << ",\"tmem_columns\":" << config.tmem_columns << "},\"tensor\":[";
    for (uint32_t sm = 0; sm < sms_.size(); ++sm) {
        if (sm) out << ',';
        const auto& array = *sms_[sm]->array;
        const auto& s = array.stats(); const auto& t = sms_[sm]->tmem.stats();
        out << "{\"backend\":\"" << array.name() << "\",\"array_clocks\":" << array.cycle()
            << ",\"tiles\":" << s.tiles << ",\"waves\":" << s.waves << ",\"products\":" << s.products
            << ",\"skipped\":" << s.skipped << ",\"scale_products\":" << s.scale_products
            << ",\"scale_accumulates\":" << s.scale_accumulates << ",\"input_stalls\":" << s.input_stalls
            << ",\"queue_stalls\":" << s.queue_stalls << ",\"tmem_bank_services\":" << t.bank_services
            << ",\"tmem_bank_stalls\":" << t.bank_stalls << ",\"tmem_broadcasts\":" << t.broadcasts << '}';
    }
    out << ']';
}

void TimingMachine::dump(std::ostream& out) const {
    dump_statistics(out);
    scoreboard_stats(out, sms_);
    register_stats(out, sms_);
    dump_memory(out);
    dump_tensor(out);
    dump_async(out);
    dump_atomic(out);
    if (machine_.config_.hardware_trace) dump_trace(out);
    out << '}';
}
}

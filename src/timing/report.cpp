#include "simt/timing.hpp"

namespace simt {
// All timing stages report the instruction's original issue location.
// 所有时序阶段均报告指令原始发射位置。
std::string timing_location(uint32_t cta, uint32_t sm, uint32_t warp, uint32_t pc) {
    return " block=" + std::to_string(cta) + " sm=" + std::to_string(sm) +
           " warp=" + std::to_string(warp) + " pc=" + std::to_string(pc);
}

namespace {
// Scheduler counters describe one physical probe port, including rejected candidates and committed lifecycle events.
// 调度器计数描述一个物理候选检查端口，包括拒绝候选及已提交的生命周期事件。
void scheduler_stats(std::ostream& out,const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"warp_schedulers\":["; bool first=true;
    for (uint32_t sm=0; sm<sms.size(); ++sm) for (uint32_t part=0; part<sms[sm]->partitions.size(); ++part) {
        const auto& unit=sms[sm]->partitions[part].scheduler; if (!unit) continue;
        if (!first) out << ',';
        first=false; const auto& s=unit->stats(); const auto state=unit->read();
        out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"physical_slots\":32"
            << ",\"limit\":" << 32/sms[sm]->partitions.size() << ",\"probe_ports\":1,\"storage_bits\":325"
            << ",\"live\":" << state.live << ",\"boundary\":" << state.boundary << ",\"memory\":" << state.memory
            << ",\"cursor\":" << state.cursor << ",\"skipped\":" << state.skipped
            << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
            << ",\"admitted\":" << s.admitted << ",\"released\":" << s.released << ",\"retired\":" << s.retired
            << ",\"probes\":" << s.probes << ",\"issued\":" << s.issued << ",\"guard_stalls\":" << s.guard_stalls
            << ",\"dependency_stalls\":" << s.dependency_stalls << ",\"boundary_stalls\":" << s.boundary_stalls
            << ",\"memory_stalls\":" << s.memory_stalls << ",\"count_stalls\":" << s.count_stalls
            << ",\"collector_stalls\":" << s.collector_stalls << ",\"completion_stalls\":" << s.completion_stalls
            << ",\"peak_inflight\":" << s.peak_inflight << ",\"inflight\":[";
        for (uint32_t slot=0; slot<32; ++slot) { if (slot) out << ','; out << state.inflight[slot]; }
        out << "]}";
    }
    out << ']';
}

TimingStats scheduling_statistics(TimingStats result,const std::vector<std::unique_ptr<TimedSm>>& sms) {
    for (const auto& sm : sms) for (const auto& partition : sm->partitions) {
        if (!partition.scheduler) continue;
        const auto& s=partition.scheduler->stats();
        result.scoreboard_stalls+=s.guard_stalls+s.dependency_stalls;
        result.boundary_stalls+=s.boundary_stalls; result.memory_order_stalls+=s.memory_stalls;
        result.collector_stalls+=s.collector_stalls; result.completion_stalls+=s.completion_stalls+s.count_stalls;
    }
    return result;
}
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

// Count accepted issue reads separately from scheduler probes and from GPR storage activity.
// 将已接受发射的读取与调度探测及普通寄存器存储活动分开计数。
void predicate_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"predicate_files\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            if (!file) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = file->stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"slots\":" << file->slots()
                << ",\"data_bits\":" << file->slots()*224 << ",\"valid_bits\":" << file->slots()*7
                << ",\"read_ports\":3,\"write_ports\":1,\"clocks\":" << s.clocks
                << ",\"active_clocks\":" << s.predicate_active_clocks << ",\"guard_reads\":" << s.guard_reads
                << ",\"source_reads\":" << s.predicate_source_reads << ",\"writes\":" << s.predicate_writes << '}';
        }
    out << ']';
}

// Collector counters reflect circuit handshakes, not a software estimate of operand availability.
// 收集器计数反映电路握手，而不是软件估算的操作数可用性。
void collector_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"collectors\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            auto* collector = file ? file->collector() : nullptr;
            if (!collector) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = collector->collector_stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"physical_entries\":32,\"limit\":" << collector->limit()
                << ",\"occupancy\":" << collector->occupancy() << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"allocated\":" << s.allocated << ",\"released\":" << s.released
                << ",\"dispatch_ports\":7,\"dispatches\":" << s.dispatches << ",\"dispatch_waits\":" << s.dispatch_waits
                << ",\"reads\":" << s.reads << ",\"bank_stalls\":" << s.bank_stalls << '}';
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

// Completion storage and retirement counts come from the shared-clock RTL handshakes.
// 完成存储与退休计数来自共用时钟的 RTL 握手。
void writeback_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"writeback_units\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            auto* unit = file ? file->writeback() : nullptr;
            if (!unit) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = unit->writeback_stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"entries\":32,\"completion_ports\":7"
                << ",\"occupancy\":" << unit->occupancy() << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"reserved\":" << s.reserved << ",\"completed\":" << s.completed << ",\"retired\":" << s.retired << '}';
        }
    out << ']';
}

// Integer counts observe real acceptance and completion edges on the connected execution circuit.
// 整数计数观测已连接执行电路的实际接收与完成边沿。
void integer_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"integer_units\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            auto* unit = file ? file->integer() : nullptr;
            if (!unit) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = unit->integer_stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"entries\":32"
                << ",\"occupancy\":" << unit->occupancy() << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"accepted\":" << s.accepted << ",\"completed\":" << s.completed << '}';
        }
    out << ']';
}

// Floating counters include physical pipeline occupancy and completed result handshakes.
// 浮点计数包含物理流水线占用和已完成结果的握手。
void floating_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"floating_units\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            auto* unit = file ? file->floating() : nullptr;
            if (!unit) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = unit->floating_stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"entries\":32,\"pipeline_stages\":4"
                << ",\"occupancy\":" << unit->occupancy() << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"accepted\":" << s.accepted << ",\"completed\":" << s.completed << '}';
        }
    out << ']';
}

void collective_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"collective_units\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t part = 0; part < sms[sm]->partitions.size(); ++part) {
            const auto& file = sms[sm]->partitions[part].registers;
            auto* unit = file ? file->collective() : nullptr;
            if (!unit) continue;
            if (!first) out << ',';
            first = false;
            const auto& s = unit->collective_stats();
            out << "{\"sm\":" << sm << ",\"partition\":" << part << ",\"backend\":\"rtl\",\"entries\":1,\"pipeline_stages\":1"
                << ",\"occupancy\":" << unit->occupancy() << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"vote_reads\":" << s.vote_reads << ",\"accepted\":" << s.accepted << ",\"completed\":" << s.completed << '}';
        }
    out << ']';
}

void control_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"warp_controls\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t slot = 0; slot < sms[sm]->controls.size(); ++slot) {
            const auto& unit = sms[sm]->controls[slot];
            if (!unit) continue;
            if (!first) out << ',';
            first = false; const auto& s = unit->stats();
            out << "{\"sm\":" << sm << ",\"slot\":" << slot << ",\"backend\":\"rtl\",\"tokens\":16,\"storage_bits\":1568"
                << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"commands\":" << s.commands << ",\"clears\":" << s.clears << ",\"woken_lanes\":" << s.woken_lanes << '}';
        }
    out << ']';
}

// Expose persistent frontend storage and real event handshakes separately from token-only counters.
// 将持久前端存储及实际事件握手与仅令牌计数分开报告。
void frontend_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"warp_frontends\":[";
    bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t slot = 0; slot < sms[sm]->controls.size(); ++slot) {
            const auto& unit = sms[sm]->controls[slot];
            if (!unit) continue;
            if (!first) out << ',';
            first = false; const auto& s = unit->frontend_stats();
            out << "{\"sm\":" << sm << ",\"slot\":" << slot << ",\"backend\":\"rtl\",\"lane_storage_bits\":3233,\"token_storage_bits\":1568"
                << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
                << ",\"issued\":" << s.issued << ",\"retire_advances\":" << s.advances
                << ",\"external_lanes\":" << s.external_lanes << ",\"resumed_lanes\":" << s.resumed_lanes
                << ",\"cta_blocked\":" << s.cta_blocked << ",\"cta_woken\":" << s.cta_woken
                << ",\"async_checked\":" << s.async_checked << ",\"async_blocked\":" << s.async_blocked << ",\"async_woken\":" << s.async_woken << '}';
        }
    out << ']';
}

// Record snapshots describe the most recent slot owner; event counters accumulate over CTA reuse.
// 记录快照描述槽位最近的所有者，事件计数跨 CTA 复用累积。
void barrier_records(std::ostream& out, const hw::CtaBarrierState& state) {
    out << ",\"threads\":" << state.threads << ",\"live_threads\":" << state.live_count << ",\"records\":[";
    for (uint32_t id = 0; id < state.records.size(); ++id) {
        if (id) out << ',';
        const auto& r = state.records[id];
        out << "{\"id\":" << id << ",\"active\":" << (r.active ? "true" : "false")
            << ",\"count\":" << r.count << ",\"arrived\":" << r.arrived << ",\"expected\":" << r.expected
            << ",\"generation\":" << r.generation << '}';
    }
    out << ']';
}
void cta_barrier_stats(std::ostream& out, const std::vector<std::unique_ptr<TimedSm>>& sms) {
    out << ",\"cta_barriers\":["; bool first = true;
    for (uint32_t sm = 0; sm < sms.size(); ++sm)
        for (uint32_t slot = 0; slot < sms[sm]->cta_tables.size(); ++slot) {
            const auto& unit = sms[sm]->cta_tables[slot];
            if (!unit) continue;
            if (!first) out << ',';
            first = false; const auto& s = unit->stats();
            out << "{\"sm\":" << sm << ",\"slot\":" << slot << ",\"backend\":\"rtl\",\"storage_bits\":18999,\"command_ports\":1"
                << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks << ",\"initializations\":" << s.initializations
                << ",\"commands\":" << s.commands << ",\"arrivals\":" << s.arrivals << ",\"exits\":" << s.exits
                << ",\"arrival_lanes\":" << s.arrival_lanes << ",\"exit_lanes\":" << s.exit_lanes << ",\"completions\":" << s.completions;
            barrier_records(out, unit->read()); out << '}';
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
    const auto stats=scheduling_statistics(stats_,sms_);
    out << "{\"model\":\"simt-research-v1\",\"cycles\":" << cycle_ - started_
        << ",\"clock_start\":" << started_ << ",\"clock_end\":" << cycle_
        << ",\"memory_counters_scope\":\"device_cumulative\""
        << ",\"issued\":" << stats.issued << ",\"retired\":" << stats.retired
        << ",\"scoreboard_stalls\":" << stats.scoreboard_stalls << ",\"boundary_stalls\":" << stats.boundary_stalls
        << ",\"collector_stalls\":" << stats.collector_stalls << ",\"register_bank_stalls\":" << stats.register_stalls
        << ",\"unit_stalls\":" << stats.unit_stalls << ",\"writeback_stalls\":" << stats.writeback_stalls
        << ",\"completion_stalls\":" << stats.completion_stalls
        << ",\"cta_barrier_stalls\":" << stats.cta_barrier_stalls
        << ",\"memory_order_stalls\":" << stats.memory_order_stalls << ",\"peak_ctas_per_sm\":" << stats.peak_ctas
        << ",\"dropped_trace\":" << stats.dropped_trace;
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
    scheduler_stats(out,sms_);
    scoreboard_stats(out, sms_);
    control_stats(out, sms_);
    frontend_stats(out, sms_);
    cta_barrier_stats(out, sms_);
    register_stats(out, sms_);
    predicate_stats(out, sms_);
    collector_stats(out, sms_);
    integer_stats(out, sms_);
    floating_stats(out, sms_);
    collective_stats(out, sms_);
    writeback_stats(out, sms_);
    dump_memory(out);
    dump_tensor(out);
    dump_async(out);
    dump_atomic(out);
    if (machine_.config_.hardware_trace) dump_trace(out);
    out << '}';
}
}

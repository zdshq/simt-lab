#include "simt/timing.hpp"
#include "simt/control.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
bool TimingMachine::dependencies(uint32_t cta, uint32_t warp, Group group, const Footprint& f) const {
    const auto& board = *warps_[cta][warp].scoreboard;
    for (const auto& use : f.reads) if (!board.readable(use.reg, use.mask)) return false;
    if (f.destination && !board.writable(*f.destination, f.write_mask)) return false;
    if (f.predicate && !board.predicate_ready(*f.predicate, f.write_mask)) return false;
    return !f.source_predicate || board.predicate_ready(*f.source_predicate, group.mask);
}

bool TimingMachine::capacity(uint32_t sm, uint32_t partition) {
    auto& file = sms_[sm]->partitions[partition].registers;
    if (file && file->writeback() && !file->writeback()->available()) {
        ++stats_.completion_stalls; return false;
    }
    if (file && file->collector()) {
        if (file->collector()->available()) return true;
        ++stats_.collector_stalls; return false;
    }
    uint32_t used = 0;
    for (const auto& [id, entry] : inflight_) {
        static_cast<void>(id);
        if (entry->sm == sm && entry->partition == partition &&
            (entry->stage == Stage::collect || entry->stage == Stage::ready)) ++used;
    }
    if (used < machine_.config_.pipeline.collectors) return true;
    ++stats_.collector_stalls;
    return false;
}

// Predicate dependencies are resolved before selecting lanes or decoding operand footprints.
// 选择参与 lane 和解码操作数占用之前，先解决谓词依赖。
bool TimingMachine::try_issue(uint32_t cta_id, uint32_t wi) {
    auto& cta = machine_.ctas_[cta_id]; auto& timed = warps_[cta_id][wi];
    if (timed.boundary) return false;
    auto group = select_warp_group(cta_id, wi);
    if (!group) return false;
    try { return issue_group(cta_id, wi, *group); }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) + timing_location(cta_id, cta.sm_id, wi, group->pc));
    }
}

// Decode and reserve only after a runnable PC group has been selected.
// 选出可运行 PC 线程组后，才进行解码和资源预约。
bool TimingMachine::issue_group(uint32_t cta_id, uint32_t wi, Group group) {
    auto& cta = machine_.ctas_[cta_id]; auto& timed = warps_[cta_id][wi];
    if (group.pc >= machine_.module_.code.size()) throw std::runtime_error("pc_out_of_range");
    const auto& instruction = machine_.module_.code[group.pc];
    if (!timed.scoreboard->predicate_ready(instruction.guard & 7, group.mask)) { ++stats_.scoreboard_stalls; return false; }
    const auto predicates = probe_predicates(cta_id, wi, instruction, group);
    const auto mask = predicates.participating;
    auto footprint = instruction_footprint(instruction, mask);
    if (footprint.boundary && timed.inflight) { ++stats_.boundary_stalls; return false; }
    if (footprint.pipe == Pipe::memory && timed.memory) { ++stats_.memory_order_stalls; return false; }
    if (!dependencies(cta_id, wi, group, footprint)) { ++stats_.scoreboard_stalls; return false; }
    if (!capacity(cta.sm_id, timed.slot % machine_.config_.pipeline.partitions)) return false;
    enqueue(cta_id, wi, instruction, group, mask, std::move(footprint), predicates.source);
    return true;
}

void TimingMachine::reserve(Inflight& entry) {
    auto& timed = warps_[entry.cta][entry.warp];
    const auto& f = entry.footprint;
    for (const auto& use : f.reads) timed.scoreboard->reserve_read(use.reg, use.mask);
    if (f.destination) timed.scoreboard->reserve_write(*f.destination, f.write_mask);
    if (f.predicate) timed.scoreboard->reserve_predicate(*f.predicate, f.write_mask);
    if (!sms_[entry.sm]->partitions[entry.partition].scheduler) {
        ++timed.inflight;
        timed.boundary |= f.boundary;
        timed.memory |= f.pipe == Pipe::memory;
    }
}

// Advance fetch PCs on ordinary issue; control instructions hold their continuation until retirement.
// 普通发射时推进取指 PC，控制指令保留后续位置直到退休。
void TimingMachine::enqueue(uint32_t cta_id, uint32_t wi, const Instruction& instruction, Group group, uint32_t mask, Footprint footprint, uint32_t predicate_source) {
    if (machine_.steps_ >= machine_.config_.max_steps) throw std::runtime_error("step_limit_exceeded");
    auto& cta = machine_.ctas_[cta_id]; auto& warp = cta.warps[wi];
    auto entry = std::make_unique<Inflight>();
    entry->id = next_++; entry->cta = cta_id; entry->warp = wi; entry->sm = cta.sm_id;
    entry->partition = warps_[cta_id][wi].slot % machine_.config_.pipeline.partitions;
    entry->group = group; entry->mask = mask; entry->instruction = instruction;
    entry->footprint = std::move(footprint);
    reserve_collector(*entry);
    reserve_writeback(*entry);
    entry->operands.valid_mask = warp.valid_mask;
    if (entry->footprint.source_predicate) entry->operands.predicates[*entry->footprint.source_predicate] = predicate_source;
    auto& file = sms_[entry->sm]->partitions[entry->partition].registers;
    if (file) file->accept_predicates(entry->footprint.source_predicate.has_value());
    reserve(*entry); issue_frontend(*entry);
    ++machine_.steps_; ++stats_.issued;
    ++machine_.op_counts_[static_cast<uint8_t>(entry->instruction.op)];
    machine_.lane_instructions_ += std::popcount(mask);
    trace(*entry, "issue");
    const auto id = entry->id; inflight_.emplace(id, std::move(entry));
}
}

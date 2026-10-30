#pragma once

namespace simt::hw::detail {
// Candidate queries settle two read muxes; one issued instruction may capture them per partition clock.
// 候选查询使两个读取多路器稳定；每个分区每拍只允许一条发射指令捕获结果。
template<class C, uint32_t S> PredicateValues RtlRegisterFile<C,S>::probe_predicates(
    uint32_t slot, uint32_t guard, uint32_t group, uint32_t source) {
    if (slot >= S || guard >= 16 || source >= 8) throw std::out_of_range("predicate_file_address");
    if (predicate_reads_) throw std::runtime_error("predicate_read_port_busy");
    const bool changed = dut_.predicate_query_slot != slot || dut_.predicate_query_guard != guard ||
        dut_.predicate_query_group != group || dut_.predicate_query_source != source;
    if (changed) {
        dut_.predicate_query_slot = slot; dut_.predicate_query_guard = guard;
        dut_.predicate_query_group = group; dut_.predicate_query_source = source; dut_.eval();
    }
    predicate_probed_ = true;
    return {dut_.predicate_participating,dut_.predicate_source_data};
}

template<class C, uint32_t S> void RtlRegisterFile<C,S>::accept_predicates(bool source) {
    if (!predicate_probed_ || predicate_reads_) throw std::runtime_error("predicate_read_not_selected");
    predicate_reads_ = source ? 3 : 1;
}

template<class C, uint32_t S> bool RtlRegisterFile<C,S>::predicate_writable() const {
    if (predicate_reads_) return false;
    if constexpr (requires { dut_.predicate_port_busy; }) return !dut_.predicate_port_busy;
    return !dut_.predicate_write_valid;
}

// Only the non-collector backend submits host predicate data; connected retirement drives the other path directly.
// 只有未启用 collector 的后端提交宿主谓词数据；直连退休电路直接驱动另一条路径。
template<class C, uint32_t S> void RtlRegisterFile<C,S>::write_predicate(
    uint32_t slot, uint32_t reg, uint32_t mask, uint32_t value) {
    if (slot >= S || reg >= 7) throw std::out_of_range("predicate_file_address");
    if (!mask) return;
    if (!predicate_writable()) throw std::runtime_error("predicate_write_port_busy");
    dut_.predicate_write_slot = slot; dut_.predicate_write_register = reg;
    dut_.predicate_write_mask = mask; dut_.predicate_write_data = value;
    dut_.predicate_write_valid = 1; dut_.eval();
    if (dut_.predicate_write_error) throw std::runtime_error("predicate_write_rejected");
}
}

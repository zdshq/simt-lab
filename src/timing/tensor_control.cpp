#include "simt/timing.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
// Physical column capacity is shared by resident CTAs, independently of their logical coordinates.
// 驻留 CTA 共享物理列容量，各 CTA 仍保留独立逻辑坐标。
void TimingMachine::allocate_tensor(Inflight& entry) {
    auto& flight = *entry.tensor; auto& sm = *sms_[entry.sm]; auto& cta = machine_.ctas_[entry.cta];
    const auto count = entry.instruction.imm;
    if (!count || count > 256 || count % 8) throw std::runtime_error("invalid_tensor_allocation");
    if (count > machine_.config_.tensor.tmem_columns - sm.allocated_columns)
        throw std::runtime_error("tensor_out_of_columns");
    flight.base = reserve_tensor_columns(cta, count);
    sm.tensor_columns[entry.cta] += count; sm.allocated_columns += count;
    flight.cursor = flight.base * 128; flight.end = (flight.base + count) * 128;
    flight.phase = TensorPhase::zero;
    write_register(entry.result, entry.instruction.rd, std::countr_zero(entry.mask), flight.base);
}

// Allocation becomes ready only after banked zero writes have all acknowledged.
// 只有分 bank 清零写入全部确认后，分配才变为就绪。
void TimingMachine::zero_tensor(Inflight& entry) {
    auto& flight = *entry.tensor;
    if (!poll_tensor_transfers(entry)) return;
    if (flight.cursor == flight.end) { flight.phase = TensorPhase::complete; return; }
    for (uint32_t lane = 0; lane < lanes && flight.cursor < flight.end; ++lane, ++flight.cursor)
        tensor_transfer(flight, 2, uint64_t{flight.cursor} * 4, 4, lane, true, 0);
}

// Validate every participating lane before any TMEM transfer can be submitted.
// 提交任何 TMEM 事务前检查全部参与 lane。
void TimingMachine::start_tensor_transfer(Inflight& entry) {
    auto& flight = *entry.tensor; auto& cta = machine_.ctas_[entry.cta]; const auto& i = entry.instruction;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(entry.mask & lane_bit(lane))) continue;
        const auto address = tensor_address(cta, read_register(entry.operands, i.ra, lane), read_register(entry.operands, i.rb, lane));
        tensor_transfer(flight, 2, address, 4, lane, i.op == Op::STT, read_register(entry.operands, i.rc, lane));
    }
    flight.phase = TensorPhase::transfer;
}

void TimingMachine::free_tensor(Inflight& entry, uint32_t address) {
    const auto count = entry.instruction.imm;
    free_tensor_columns(machine_.ctas_[entry.cta], address, count);
    sms_[entry.sm]->allocated_columns -= count; sms_[entry.sm]->tensor_columns[entry.cta] -= count;
    entry.tensor->phase = TensorPhase::complete;
}

void TimingMachine::start_tensor_descriptor(Inflight& entry, uint32_t address) {
    machine_.ctas_[entry.cta].shared.check(address, 80, 16);
    for (uint32_t word = 0; word < 20; ++word) tensor_transfer(*entry.tensor, 1, uint64_t{address} + word * 4, 4, word);
    entry.tensor->phase = TensorPhase::descriptor;
}

void TimingMachine::start_tensor(Inflight& entry) {
    const auto& i = entry.instruction;
    if (!entry.mask) { entry.tensor->phase = TensorPhase::complete; return; }
    if (i.op == Op::LDT || i.op == Op::STT) { start_tensor_transfer(entry); return; }
    if (std::popcount(entry.mask) != 1) throw std::runtime_error("tensor_single_issuer_required");
    if (i.op == Op::TMEM_ALLOC) { allocate_tensor(entry); return; }
    const auto address = read_register(entry.operands, i.ra, std::countr_zero(entry.mask));
    if (i.op == Op::TMEM_FREE) free_tensor(entry, address);
    else start_tensor_descriptor(entry, address);
}

// One state transition per service clock keeps frontend and memory completion order explicit.
// 每个服务时钟只推进一次状态转移，使前端和内存完成顺序明确。
void TimingMachine::advance_tensor(Inflight& entry) {
    auto& flight = *entry.tensor;
    switch (flight.phase) {
    case TensorPhase::start: start_tensor(entry); break;
    case TensorPhase::zero: zero_tensor(entry); break;
    case TensorPhase::descriptor: tensor_descriptor(entry); break;
    case TensorPhase::initial: initial_tensor_tile(entry); break;
    case TensorPhase::metadata: tensor_metadata(entry); break;
    case TensorPhase::operands: decode_tensor_operands(entry); break;
    case TensorPhase::feed: feed_tensor(entry); break;
    case TensorPhase::drain: drain_tensor(entry); break;
    case TensorPhase::store: store_tensor(entry); break;
    case TensorPhase::complete: entry.stage = Stage::writeback; trace(entry, "tensor_complete"); break;
    case TensorPhase::transfer:
        if (!poll_tensor_transfers(entry)) break;
        if (entry.instruction.op == Op::LDT) for (uint32_t lane = 0; lane < lanes; ++lane)
            if (entry.mask & lane_bit(lane)) write_register(entry.result, entry.instruction.rd, lane, flight.fetched[lane]);
        flight.phase = TensorPhase::complete; break;
    }
}

void TimingMachine::poll_tensor(Inflight& entry) {
    try { advance_tensor(entry); }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) + timing_location(entry.cta, entry.sm, entry.warp, entry.group.pc));
    }
}
}

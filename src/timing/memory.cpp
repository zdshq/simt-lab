#include "simt/timing.hpp"
#include "simt/async.hpp"
#include <bit>
#include <stdexcept>

namespace simt {
namespace {
uint32_t width(uint32_t mode) {
    if (mode > 4) throw std::runtime_error("invalid_load_store_mode");
    return mode < 2 ? 1 : mode < 4 ? 2 : 4;
}

uint32_t extended(uint32_t value, uint32_t mode) {
    if (mode == 1 && (value & 0x80)) return value | 0xFFFFFF80;
    if (mode == 3 && (value & 0x8000)) return value | 0xFFFF8000;
    return value;
}

// Generic pointers are split by address space before transactions enter physical memory units.
// 通用指针先按地址空间拆分，再让事务进入物理内存单元。
uint32_t space(Op op, uint64_t& address) {
    if (op == Op::LDG || op == Op::STG) return 0;
    if (op == Op::LDS || op == Op::STS) return 1;
    if (op == Op::LDL || op == Op::STL) return 2;
    if (op == Op::LDC) return 3;
    if (address >= 0xF0000000 && address < 0xF0010000) { address -= 0xF0000000; return 1; }
    if (address >= 0xF1000000 && address < 0xF1004000) { address -= 0xF1000000; return 2; }
    return 0;
}

hw::WarpAccess& flight(std::vector<MemoryFlight>& flights, uint32_t space, bool store, uint32_t size) {
    for (auto& part : flights) if (part.space == space) return part.access;
    hw::WarpAccess access; access.mask = 0; access.width = size; access.write = store;
    flights.push_back({space, access, {}, false});
    return flights.back().access;
}
}

// Validate the complete scatter before submitting any global or shared store fragment.
// 提交任何 global 或 shared 写片段前，先检查完整 scatter。
void TimingMachine::prepare_memory(Inflight& entry) {
    const auto& i = entry.instruction; auto& cta = machine_.ctas_[entry.cta];
    if (i.op == Op::ATOM || i.op == Op::RED) { prepare_atomic(entry); return; }
    const auto size = width(i.mode);
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (!(entry.mask & lane_bit(lane))) continue;
        const auto signed_address = int64_t{read_register(entry.operands, i.ra, lane)} + std::bit_cast<int32_t>(i.imm);
        if (signed_address < 0 || signed_address > UINT32_MAX) throw std::runtime_error("memory_address_overflow");
        auto address = static_cast<uint64_t>(signed_address);
        const auto kind = space(i.op, address);
        const auto& storage = kind == 0 ? *machine_.global_ : kind == 1 ? cta.shared :
            kind == 2 ? cta.local[entry.warp * lanes + lane] : machine_.constant_;
        storage.check(address, size, size);
        if (entry.footprint.store && kind == 1) check_shared_write(cta, address, size);
        if (kind == 2) address = hw::LocalAddressSpace::translate(local_base_ + entry.cta * 32 + entry.warp, lane, static_cast<uint32_t>(address));
        auto& access = flight(entry.memory, kind, entry.footprint.store, size);
        access.mask |= lane_bit(lane); access.address[lane] = address;
        access.value[lane] = read_register(entry.operands, i.rb, lane);
    }
}

void TimingMachine::finish_memory(Inflight& entry, const MemoryFlight& flight, const std::array<uint32_t, 32>& values) {
    if (entry.footprint.store && entry.instruction.op != Op::ATOM) return;
    for (uint32_t lane = 0; lane < lanes; ++lane)
        if (flight.access.mask & lane_bit(lane))
            write_register(entry.result, entry.instruction.rd, lane,
                           entry.instruction.op == Op::ATOM ? values[lane] : extended(values[lane], entry.instruction.mode));
}

// Failed admissions retry without replaying accepted fragments or reading backing memory directly.
// 准入失败时重试，不重放已接受片段，也不直接读取底层内存。
void TimingMachine::poll_memory(Inflight& entry) {
    bool done = true;
    auto& sm = *sms_[entry.sm];
    for (auto& flight : entry.memory) {
        if (flight.done) continue;
        auto& port = flight.space == 1 ? *sm.shared : static_cast<hw::SharedPort&>(sm.constant);
        auto& backing = flight.space == 1 ? machine_.ctas_[entry.cta].shared : machine_.constant_;
        if (!flight.ticket) {
            flight.ticket = (flight.space == 0 || flight.space == 2) ? submit_memory(entry.sm, flight.access) : port.submit(flight.access, &backing);
            if (flight.ticket) {
                trace(entry, "memory_submit");
                if (flight.access.atomic) trace(entry, "atomic_submit");
                if (flight.space == 2) trace(entry, "local_submit");
            }
        }
        const auto result = !flight.ticket ? std::optional<std::array<uint32_t, 32>>{} :
            (flight.space == 0 || flight.space == 2) ? memory_->take(*flight.ticket) : port.take(*flight.ticket);
        if (result) { finish_memory(entry, flight, *result); flight.done = true; trace(entry, "memory_complete"); }
        done &= flight.done;
    }
    if (done) entry.stage = Stage::writeback;
}
}

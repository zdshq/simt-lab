#pragma once
#include "bench.hpp"
#include "Vsector_cache.h"
#include "simt/hardware_memory.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <vector>

using simt::hw::Packet;
using simt::hw::Sector;

template<class Words> Sector unpack(const Words& words) {
    Sector data{};
    for (uint32_t byte = 0; byte < 32; ++byte) data[byte] = uint8_t(words[byte/4] >> (8*(byte%4)));
    return data;
}

template<class Words> void pack(Words& words, const Sector& data) {
    for (uint32_t word = 0; word < 8; ++word) {
        words[word] = 0;
        for (uint32_t byte = 0; byte < 4; ++byte) words[word] |= uint32_t(data[word*4+byte]) << (8*byte);
    }
}

struct LowerRequest { uint32_t id; Packet packet; uint64_t due; };

// The independently scheduled lower memory can reorder fills and stall either interface.
// 独立调度的下层内存可以重排填充，并阻塞任一接口。
class CacheBench : public Bench<Vsector_cache> {
public:
    simt::Memory backing{uint64_t{1} << 40};
    std::map<uint32_t, Sector> results;
    std::vector<Packet> downstream;
    uint64_t reads = 0, writes = 0;
    uint32_t delay = 9, seed = 71;
    bool random_stalls = false, send_memory = true, send_fill = true, take_response = true;
    explicit CacheBench(const char* path) : Bench(path) {}
    void step();
    bool submit(uint32_t id, const Packet& packet);
    Sector wait(uint32_t id);
    void drain();
    void clear();
private:
    std::deque<LowerRequest> pending_;
    std::optional<LowerRequest> filling_;
    std::optional<LowerRequest> held_memory_;
    std::optional<std::pair<uint32_t, Sector>> held_response_;
    uint32_t random();
    void prepare();
    void accept_memory();
    void check_stability();
    void transfer();
    Sector read_sector(uint64_t address) const;
};

inline uint32_t CacheBench::random() { seed = seed*1664525u+1013904223u; return seed; }

inline Sector CacheBench::read_sector(uint64_t address) const {
    Sector value{};
    for (uint32_t byte = 0; byte < 32; ++byte) value[byte] = uint8_t(backing.read(address+byte, 1));
    return value;
}

// A fill captures data at memory completion, rather than at request admission.
// 填充在内存完成时捕获数据，而不是在请求准入时捕获。
inline void CacheBench::prepare() {
    dut.mem_ready = send_memory && (!random_stalls || random()%4 != 0);
    dut.rsp_ready = take_response && (!random_stalls || random()%3 != 0);
    if (!filling_ && send_fill) {
        auto chosen = pending_.end();
        for (auto at = pending_.begin(); at != pending_.end(); ++at)
            if (at->due <= cycle && (chosen == pending_.end() || at->due < chosen->due)) chosen = at;
        if (chosen != pending_.end()) {
            filling_ = *chosen; pending_.erase(chosen);
            const auto& p = filling_->packet;
            if (p.write) for (uint32_t byte = 0; byte < 32; ++byte)
                if (p.byte_mask & (uint32_t{1} << byte)) backing.write(p.address+byte, 1, p.data[byte]);
            pack(dut.fill_data, read_sector(p.address));
        }
    }
    dut.fill_valid = bool(filling_);
    dut.fill_id = filling_ ? filling_->id : 0;
    dut.eval();
}

inline void CacheBench::accept_memory() {
    if (!dut.mem_valid || !dut.mem_ready) return;
    Packet packet{dut.mem_address, bool(dut.mem_write), unpack(dut.mem_data), dut.mem_mask};
    pending_.push_back({dut.mem_id, packet, cycle+delay+(random_stalls ? random()%17 : 0)});
    downstream.push_back(packet);
    if (packet.write) ++writes; else ++reads;
}

inline void CacheBench::check_stability() {
    if (held_memory_) {
        const auto& old = *held_memory_;
        require(dut.mem_valid && dut.mem_id == old.id && dut.mem_address == old.packet.address &&
                bool(dut.mem_write) == old.packet.write && unpack(dut.mem_data) == old.packet.data &&
                dut.mem_mask == old.packet.byte_mask, "downstream changed while stalled");
    }
    if (held_response_) require(dut.rsp_valid && dut.rsp_id == held_response_->first &&
                               unpack(dut.rsp_data) == held_response_->second, "response changed while stalled");
    held_memory_.reset(); held_response_.reset();
    if (dut.mem_valid && !dut.mem_ready)
        held_memory_ = LowerRequest{dut.mem_id, {dut.mem_address, bool(dut.mem_write), unpack(dut.mem_data), dut.mem_mask}, 0};
    if (dut.rsp_valid && !dut.rsp_ready) held_response_ = std::make_pair(dut.rsp_id, unpack(dut.rsp_data));
}

inline void CacheBench::step() {
    prepare(); transfer();
}

inline void CacheBench::transfer() {
    check_stability(); accept_memory();
    if (dut.rsp_valid && dut.rsp_ready) {
        require(!results.contains(dut.rsp_id), "duplicate response ID");
        results[dut.rsp_id] = unpack(dut.rsp_data);
    }
    const bool returned = dut.fill_valid && dut.fill_ready;
    tick();
    if (returned) filling_.reset();
}

inline bool CacheBench::submit(uint32_t id, const Packet& packet) {
    dut.req_valid = 1; dut.req_address = packet.address; dut.req_write = packet.write;
    dut.req_id = id; dut.req_mask = packet.byte_mask; pack(dut.req_data, packet.data);
    prepare();
    const bool accepted = dut.req_ready;
    transfer();
    dut.req_valid = 0; dut.eval();
    return accepted;
}

inline Sector CacheBench::wait(uint32_t id) {
    for (uint32_t limit = 0; limit < 10000 && !results.contains(id); ++limit) step();
    require(results.contains(id), "response timeout");
    auto value = results.at(id); results.erase(id); return value;
}

inline void CacheBench::drain() {
    for (uint32_t limit = 0; limit < 10000 && !dut.idle; ++limit) step();
    require(dut.idle, "cache failed to drain");
}

inline void CacheBench::clear() {
    drain(); dut.invalidate_valid = 1; dut.invalidate_all = 1; dut.eval();
    require(dut.invalidate_ready, "idle cache refused invalidation");
    step(); dut.invalidate_valid = 0; dut.eval();
    drain();
}

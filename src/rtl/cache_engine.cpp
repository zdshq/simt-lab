#include "simt/cache_engine.hpp"
#include "Vsimt_l1.h"
#include "Vsimt_l2.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace simt::hw {
namespace {
template<class Words> Sector unpack(const Words& words) {
    Sector data{};
    for (uint32_t byte = 0; byte < sector_bytes; ++byte)
        data[byte] = uint8_t(words[byte/4] >> (8*(byte%4)));
    return data;
}

template<class Words> void pack(Words& words, const Sector& data) {
    for (uint32_t word = 0; word < 8; ++word) {
        words[word] = 0;
        for (uint32_t byte = 0; byte < 4; ++byte) words[word] |= uint32_t(data[word*4+byte]) << (8*byte);
    }
}

struct Invalidation { bool all; uint64_t first, last; };

// Only transport tickets and bounded returned payloads live here; all cache bytes/tags live in the DUT.
// 此处只保存传输票据和有限返回载荷；全部缓存字节和 tag 均在 DUT 内。
template<class Circuit> class RtlCache final : public CachePort {
public:
    explicit RtlCache(SectorPort& lower, CacheConfig config) : lower_(lower), config_(config) {
        dut_.clk = 0; dut_.reset = 1;
        dut_.req_valid = 0; dut_.mem_ready = 0; dut_.fill_valid = 0;
        dut_.rsp_ready = 0; dut_.invalidate_valid = 0; dut_.eval();
    }
    ~RtlCache() override { dut_.final(); }
    std::optional<Ticket> submit(const Packet& packet) override;
    void tick() override;
    void invalidate() override;
    void check_invalidation(uint64_t address, uint64_t size) const override;
    void invalidate_range(uint64_t address, uint64_t size) override;
    const CacheStats& stats() const override { return stats_; }
    const CacheConfig& config() const override { return config_; }
    size_t miss_count() const override { return std::popcount(uint32_t(dut_.active_misses)); }
    std::string_view name() const override { return "rtl"; }
private:
    Circuit dut_;
    SectorPort& lower_;
    CacheConfig config_;
    CacheStats stats_;
    std::array<Ticket, 8> ids_{};
    std::array<std::optional<Ticket>, 4> downstream_{};
    std::deque<Invalidation> invalidations_;
    void prepare_memory();
    void prepare_fill();
    void prepare_invalidation();
    void count_rejection();
    void observe();
    bool maintenance() const { return !invalidations_.empty() || dut_.invalidate_busy; }
};

template<class C> void RtlCache<C>::count_rejection() {
    if (dut_.reject_response) ++stats_.response_stalls;
    else if (dut_.reject_mshr) ++stats_.mshr_stalls;
    else if (dut_.reject_reservation) ++stats_.reservation_stalls;
    else ++stats_.port_stalls;
}

// Submission reserves one stable input port; it never clocks a request into the RTL secretly.
// 提交保留一个稳定输入端口，不会偷偷推进时钟把请求送入 RTL。
template<class C> std::optional<Ticket> RtlCache<C>::submit(const Packet& packet) {
    check_packet(packet);
    if (outstanding() == ids_.size()) { ++stats_.response_stalls; return {}; }
    if (dut_.req_valid || maintenance()) { ++stats_.port_stalls; return {}; }
    dut_.req_address = packet.address; dut_.req_write = packet.write; dut_.req_mask = packet.byte_mask;
    pack(dut_.req_data, packet.data); dut_.eval();
    if (!dut_.req_ready) { count_rejection(); return {}; }
    size_t id = 0;
    while (id < ids_.size() && responses_.contains(ids_[id])) ++id;
    if (id == ids_.size()) throw std::runtime_error("cache_rtl_response_ownership");
    const auto ticket = reserve(); ids_[id] = ticket;
    dut_.req_id = id; dut_.req_valid = 1; dut_.eval();
    return ticket;
}

template<class C> void RtlCache<C>::prepare_memory() {
    dut_.mem_ready = 0;
    if (!dut_.mem_valid) return;
    if (downstream_[dut_.mem_id]) throw std::runtime_error("cache_rtl_duplicate_mshr");
    auto ticket = lower_.submit({dut_.mem_address, bool(dut_.mem_write), unpack(dut_.mem_data), dut_.mem_mask});
    if (!ticket) { ++stats_.lower_stalls; return; }
    downstream_[dut_.mem_id] = ticket; dut_.mem_ready = 1;
}

template<class C> void RtlCache<C>::prepare_fill() {
    if (dut_.fill_valid) return;
    for (uint32_t id = 0; id < downstream_.size(); ++id) {
        if (!downstream_[id]) continue;
        auto data = lower_.take(*downstream_[id]);
        if (!data) continue;
        dut_.fill_id = id; dut_.fill_valid = 1; pack(dut_.fill_data, *data);
        return;
    }
}

template<class C> void RtlCache<C>::prepare_invalidation() {
    dut_.invalidate_valid = !invalidations_.empty() && !dut_.req_valid;
    if (!dut_.invalidate_valid) return;
    const auto& command = invalidations_.front();
    dut_.invalidate_all = command.all;
    dut_.invalidate_first = command.first; dut_.invalidate_last = command.last;
}

template<class C> void RtlCache<C>::observe() {
    if (dut_.req_valid && dut_.req_ready) {
        if (dut_.req_write) ++stats_.writes; else ++stats_.reads;
    }
    stats_.line_misses += dut_.event_line_miss;
    stats_.fill_bytes += sector_bytes * uint64_t(dut_.event_read_fill);
    if (dut_.fill_valid && !dut_.fill_ready) ++stats_.fill_stalls;
}

// One edge per shared device clock; response transport remains bounded by eight live tickets.
// 每个共享设备时钟只推进一个边沿；响应传输受八个存活票据限制。
template<class C> void RtlCache<C>::tick() {
    dut_.clk = 0; dut_.eval();
    const bool reset = dut_.reset;
    if (!reset) { prepare_fill(); prepare_memory(); prepare_invalidation(); }
    dut_.rsp_ready = !reset; dut_.eval();
    const bool request = dut_.req_valid && dut_.req_ready;
    const bool fill = dut_.fill_valid && dut_.fill_ready;
    const bool invalidate = dut_.invalidate_valid && dut_.invalidate_ready;
    const bool response = dut_.rsp_valid && dut_.rsp_ready;
    const auto id = dut_.rsp_id;
    const auto data = unpack(dut_.rsp_data);
    if (!reset) observe();
    dut_.clk = 1; dut_.eval(); ++cycle_; dut_.clk = 0; dut_.eval();
    if (reset) dut_.reset = 0;
    if (request) dut_.req_valid = 0;
    if (fill) { downstream_[dut_.fill_id].reset(); dut_.fill_valid = 0; }
    if (invalidate) { invalidations_.pop_front(); dut_.invalidate_valid = 0; }
    if (response) {
        if (id >= ids_.size() || !responses_.contains(ids_[id]) || responses_.at(ids_[id]).due != UINT64_MAX)
            throw std::runtime_error("cache_rtl_unowned_response");
        complete(ids_[id], data);
    }
    dut_.mem_ready = 0; dut_.eval();
    stats_.hits = dut_.hits; stats_.sector_misses = dut_.misses;
    stats_.merges = dut_.merges; stats_.evictions = dut_.evictions;
}

template<class C> void RtlCache<C>::check_invalidation(uint64_t address, uint64_t size) const {
    if (!size) return;
    if (address > UINT64_MAX-size) throw std::runtime_error("invalid_invalidation_range");
    if (invalidations_.size() == 64) throw std::runtime_error("cache_invalidation_queue_full");
}

template<class C> void RtlCache<C>::invalidate_range(uint64_t address, uint64_t size) {
    check_invalidation(address, size);
    if (size) invalidations_.push_back({false, address/sector_bytes, (address+size-1)/sector_bytes});
}

template<class C> void RtlCache<C>::invalidate() {
    if (outstanding() || dut_.req_valid || miss_count()) throw std::runtime_error("invalidate_busy_cache");
    // A drained full flush subsumes queued ranges, but still executes its physical scan on later clocks.
    // 已排空的整体失效包含待处理范围，但仍在后续时钟执行物理扫描。
    invalidations_.clear(); invalidations_.push_back({true, 0, 0});
}
}

std::unique_ptr<CachePort> make_rtl_cache(SectorPort& lower, CacheConfig config, CacheLevel level) {
    const CacheConfig original = level == CacheLevel::l1 ? CacheConfig{} : CacheConfig{262144,128,8,8,12,32,32,256};
    if (config.bytes != original.bytes || config.line_bytes != original.line_bytes || config.ways != original.ways ||
        config.banks != original.banks || config.hit_latency != original.hit_latency || config.miss_entries != original.miss_entries ||
        config.merge_limit != original.merge_limit || config.capacity != original.capacity)
        throw std::runtime_error("cache_rtl_requires_default_geometry");
    const auto physical = rtl_cache_config(level);
    if (level == CacheLevel::l1) return std::make_unique<RtlCache<Vsimt_l1>>(lower, physical);
    return std::make_unique<RtlCache<Vsimt_l2>>(lower, physical);
}
}

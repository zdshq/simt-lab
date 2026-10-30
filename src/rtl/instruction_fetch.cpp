#include "simt/instruction_fetch.hpp"
#include "Vsimt_instruction_fetch.h"
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlInstructionFetch final : public InstructionFetch {
public:
    RtlInstructionFetch(MemoryHierarchy& memory,const InstructionMapping& image,uint32_t sm,uint32_t partitions,uint64_t owner);
    ~RtlInstructionFetch() override { dut_.final(); }
    std::optional<InstructionWords> lookup(uint32_t partition,uint32_t pc) override;
    void tick() override;
    bool busy() const override { return dut_.busy; }
    const InstructionFetchStats& stats() const override { return stats_; }
private:
    Vsimt_instruction_fetch dut_;
    MemoryHierarchy& memory_;
    uint64_t base_,owner_;
    uint32_t sm_;
    std::optional<Ticket> memory_ticket_;
    std::optional<InstructionWords> memory_reply_;
    InstructionFetchStats stats_;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void drive_memory();
    void submit_memory();
    void accept_memory();
    void sample_events();
};

RtlInstructionFetch::RtlInstructionFetch(MemoryHierarchy& memory,const InstructionMapping& image,
    uint32_t sm,uint32_t partitions,uint64_t owner) : memory_(memory),base_(image.address()),owner_(owner),sm_(sm) {
    if (!partitions || partitions>8 || sm>=64)
        throw std::out_of_range("invalid_instruction_fetch_configuration");
    static_cast<void>(memory.l1_cache(sm));
    dut_.partition_limit=partitions; dut_.program_words=image.words(); dut_.query_valid=0;
    dut_.memory_request_ready=0; dut_.memory_response_valid=0;
    dut_.clk=0; dut_.reset=1; edge(); dut_.reset=0; dut_.eval();
}

std::optional<InstructionWords> RtlInstructionFetch::lookup(uint32_t partition,uint32_t pc) {
    if (partition>=dut_.partition_limit) throw std::out_of_range("instruction_fetch_partition");
    const auto bit=uint32_t{1}<<partition;
    if (dut_.query_valid&bit) throw std::runtime_error("instruction_fetch_already_queried");
    dut_.query_valid|=bit; dut_.query_pcs[partition]=pc; dut_.eval(); ++stats_.queries[partition];
    if (dut_.address_errors&bit) { ++stats_.address_errors[partition]; throw std::runtime_error("pc_out_of_range"); }
    if (!(dut_.response_valid&bit)) { ++stats_.stalls[partition]; return {}; }
    InstructionWords words;
    for (uint32_t word=0; word<4; ++word) words[word]=dut_.response_words[partition*4+word];
    ++stats_.delivered[partition]; return words;
}

// Returned words come exclusively from completed hierarchy requests, without a second backing read.
// 返回机器字仅来自已完成的层次请求，不再次读取后备存储。
void RtlInstructionFetch::drive_memory() {
    dut_.memory_request_ready=0;
    if (memory_ticket_) {
        if (const auto words=memory_.take(*memory_ticket_)) {
            memory_reply_=InstructionWords{(*words)[0],(*words)[1],(*words)[2],(*words)[3]};
            memory_ticket_.reset();
        }
    }
    dut_.memory_response_valid=bool(memory_reply_);
    if (dut_.memory_response_valid)
        for (uint32_t word=0; word<4; ++word) dut_.memory_response_words[word]=(*memory_reply_)[word];
    dut_.eval();
    submit_memory();
}

// One accepted 16-byte request bypasses data L1 and competes for the shared hierarchy capacity.
// 一个已接受的 16 字节请求绕过数据 L1，竞争共享层次容量。
void RtlInstructionFetch::submit_memory() {
    if (!dut_.memory_request_valid || memory_ticket_ || memory_reply_) return;
    WarpAccess access; access.mask=15;
    for (uint32_t word=0; word<4; ++word) access.address[word]=base_+uint64_t{dut_.memory_request_pc}*16+word*4;
    memory_ticket_=memory_.submit(sm_,access,owner_,true);
    dut_.memory_request_ready=bool(memory_ticket_);
    if (!memory_ticket_) ++stats_.memory_stalls;
    dut_.eval();
}

void RtlInstructionFetch::accept_memory() {
    if (dut_.memory_response_valid && dut_.memory_response_ready) { memory_reply_.reset(); ++stats_.memory_responses; }
    if (dut_.memory_request_valid && dut_.memory_request_ready) ++stats_.memory_requests;
}
void RtlInstructionFetch::sample_events() {
    for (uint32_t part=0; part<8; ++part) {
        const auto bit=uint32_t{1}<<part;
        stats_.reads[part]+=bool(dut_.read_events&bit); stats_.hits[part]+=bool(dut_.hit_events&bit);
        stats_.misses[part]+=bool(dut_.miss_events&bit); stats_.fills[part]+=bool(dut_.fill_events&bit);
    }
}
void RtlInstructionFetch::tick() {
    drive_memory(); sample_events(); accept_memory(); edge(); ++stats_.clocks;
    dut_.query_valid=0; dut_.memory_request_ready=0; dut_.memory_response_valid=0; dut_.eval();
}
}
std::unique_ptr<InstructionFetch> make_instruction_fetch(MemoryHierarchy& memory,const InstructionMapping& image,
    uint32_t sm,uint32_t partitions,uint64_t owner) {
    return std::make_unique<RtlInstructionFetch>(memory,image,sm,partitions,owner);
}
}

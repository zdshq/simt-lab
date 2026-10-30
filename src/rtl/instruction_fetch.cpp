#include "simt/instruction_fetch.hpp"
#include "Vsimt_instruction_fetch.h"
#include <stdexcept>

namespace simt::hw {
namespace {
class RtlInstructionFetch final : public InstructionFetch {
public:
    RtlInstructionFetch(std::span<const InstructionWords> image,uint32_t partitions);
    ~RtlInstructionFetch() override { dut_.final(); }
    std::optional<InstructionWords> lookup(uint32_t partition,uint32_t pc) override;
    void tick() override;
    bool busy() const override { return dut_.busy; }
    const InstructionFetchStats& stats() const override { return stats_; }
private:
    struct Reply { uint64_t due; InstructionWords words; };
    Vsimt_instruction_fetch dut_;
    std::span<const InstructionWords> image_;
    std::optional<Reply> memory_reply_;
    InstructionFetchStats stats_;
    void edge() { dut_.clk=1; dut_.eval(); dut_.clk=0; dut_.eval(); }
    void drive_memory();
    void accept_memory();
    void sample_events();
};

RtlInstructionFetch::RtlInstructionFetch(std::span<const InstructionWords> image,uint32_t partitions) : image_(image) {
    if (!partitions || partitions>8 || !image.size() || image.size()>(uint64_t{1}<<32))
        throw std::out_of_range("invalid_instruction_fetch_configuration");
    dut_.partition_limit=partitions; dut_.program_words=image.size(); dut_.query_valid=0;
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

// Only refill requests access the backing image; a hit never reads host instruction bytes.
// 只有补充请求访问后备映像；命中不会读取宿主指令字节。
void RtlInstructionFetch::drive_memory() {
    dut_.memory_request_ready=!memory_reply_;
    dut_.memory_response_valid=memory_reply_ && memory_reply_->due<=stats_.clocks;
    if (dut_.memory_response_valid)
        for (uint32_t word=0; word<4; ++word) dut_.memory_response_words[word]=memory_reply_->words[word];
    dut_.eval();
}
void RtlInstructionFetch::accept_memory() {
    if (dut_.memory_response_valid && dut_.memory_response_ready) { memory_reply_.reset(); ++stats_.memory_responses; }
    if (dut_.memory_request_valid && dut_.memory_request_ready) {
        if (memory_reply_) throw std::runtime_error("instruction_memory_request_overlap");
        const auto pc=dut_.memory_request_pc;
        memory_reply_=Reply{stats_.clocks+8,pc<image_.size() ? image_[pc] : InstructionWords{}};
        ++stats_.memory_requests;
    }
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
std::unique_ptr<InstructionFetch> make_instruction_fetch(std::span<const InstructionWords> image,uint32_t partitions) {
    return std::make_unique<RtlInstructionFetch>(image,partitions);
}
}

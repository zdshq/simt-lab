#include "simt/timing.hpp"

namespace simt {
namespace {
// DMA counters come from clocked port events, not a software mirror of fragment progress.
// DMA 计数来自时钟采样的端口事件，不使用片段进度的软件镜像。
void dma_statistics(std::ostream& out, const hw::DmaEngine* unit) {
    out << ",\"dma_backend\":\"" << (unit ? "rtl" : "cpp") << '"';
    if (!unit) return;
    const auto& s=unit->stats();
    out << ",\"dma_engine\":{\"command_slots\":64,\"fragment_slots\":64,\"payload_bits\":65536"
        << ",\"commands\":" << unit->commands() << ",\"fragments\":" << unit->fragments()
        << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks
        << ",\"submitted\":" << s.submitted << ",\"released\":" << s.released
        << ",\"allocated\":" << s.fragments << ",\"reads\":" << s.reads << ",\"writes\":" << s.writes
        << ",\"read_returns\":" << s.read_returns << ",\"write_returns\":" << s.write_returns
        << ",\"read_bytes\":" << s.read_bytes << ",\"write_bytes\":" << s.write_bytes
        << ",\"completion_claims\":" << s.completion_claims << ",\"fragment_stalls\":" << s.fragment_stalls
        << ",\"read_stalls\":" << s.read_stalls << ",\"write_stalls\":" << s.write_stalls
        << ",\"completion_stalls\":" << s.completion_stalls << ",\"peak_commands\":" << s.peak_commands
        << ",\"peak_fragments\":" << s.peak_fragments << '}';
}

AsyncStats copy_statistics(const TimedSm& sm) {
    auto result=sm.async_stats;
    if (sm.dma) {
        const auto& s=sm.dma->stats();
        result.read_bytes=s.read_bytes; result.write_bytes=s.write_bytes;
        result.fragments=s.fragments; result.fragment_stalls=s.fragment_stalls; result.peak_commands=s.peak_commands;
    }
    return result;
}

// Hardware events describe table work, separately from DMA payload traffic.
// 硬件事件描述事务表工作，与 DMA 载荷流量分开统计。
void transaction_statistics(std::ostream& out, const hw::TransactionTable* unit) {
    out << ",\"transaction_backend\":\"" << (unit ? "rtl" : "cpp") << '"';
    if (!unit) return;
    const auto& s=unit->stats();
    out << ",\"transaction_table\":{\"physical_slots\":1024,\"record_bits\":224,\"occupancy\":" << unit->occupancy()
        << ",\"clocks\":" << s.clocks << ",\"active_clocks\":" << s.active_clocks << ",\"commands\":" << s.commands
        << ",\"search_reads\":" << s.searches << ",\"publications\":" << s.publications << ",\"responses\":" << s.responses
        << ",\"errors\":" << s.errors << ",\"phases\":" << s.phases << ",\"publication_stalls\":" << s.publication_stalls << '}';
}
}
// Counters distinguish submitted commands, payload acknowledgements, and published completions.
// 计数分别区分已提交命令、载荷确认和已发布完成。
void TimingMachine::dump_async(std::ostream& out) const {
    const auto& config = machine_.config_.async;
    out << ",\"async_config\":{\"commands\":" << config.commands << ",\"fragments\":" << config.fragments
        << ",\"chunk_bytes\":" << config.chunk_bytes << ",\"barrier_slots\":" << config.barrier_slots
        << ",\"barrier_latency\":" << config.barrier_latency << "},\"async\":[";
    for (uint32_t sm = 0; sm < sms_.size(); ++sm) {
        if (sm) out << ',';
        const auto s = copy_statistics(*sms_[sm]);
        out << "{\"submitted\":" << s.copies << ",\"completed\":" << s.completed
            << ",\"read_bytes\":" << s.read_bytes << ",\"write_bytes\":" << s.write_bytes
            << ",\"fragments\":" << s.fragments << ",\"command_stalls\":" << s.command_stalls
            << ",\"fragment_stalls\":" << s.fragment_stalls << ",\"memory_stalls\":" << s.memory_stalls
            << ",\"peak_commands\":" << s.peak_commands << ",\"barrier_requests\":" << s.barrier_requests
            << ",\"barrier_stalls\":" << s.barrier_stalls + (sms_[sm]->execution_arbiter ? sms_[sm]->execution_arbiter->stats().shared_stalls[6] : 0)
            << ",\"wait_queries\":" << s.wait_queries
            << ",\"phase_completions\":" << s.phase_completions;
        transaction_statistics(out,sms_[sm]->transactions.get());
        dma_statistics(out,sms_[sm]->dma.get()); out << '}';
    }
    out << ']';
}
}

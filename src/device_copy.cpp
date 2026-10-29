#include "simt/device.hpp"
#include <algorithm>

namespace simt {
namespace {
bool reads_device(CommandKind kind) { return kind == CommandKind::d2h || kind == CommandKind::d2d; }
bool writes_device(CommandKind kind) { return kind != CommandKind::d2h; }

// Directional chunks give memmove semantics without retaining a full device source snapshot.
// 定向数据块提供 memmove 语义，不保留整个设备源快照。
bool copy_backwards(const Command& command) {
    return command.kind == CommandKind::d2d && command.destination > command.source &&
           uint64_t{command.destination} < uint64_t{command.source} + command.size;
}

// The research host link has eight setup clocks and transfers sixteen bytes per clock.
// 研究模型的主机链路需要八拍建立延迟，每拍传输十六字节。
uint64_t link_delay(uint32_t bytes) { return 8 + (bytes + 15) / 16; }

hw::WarpAccess copy_access(const Command& command, const CopyTransfer& transfer) {
    hw::WarpAccess access;
    access.width = transfer.width;
    access.write = transfer.stage == CopyTransfer::Stage::write;
    const auto count = transfer.bytes / transfer.width;
    access.mask = count == 32 ? UINT32_MAX : (uint32_t{1} << count) - 1;
    const auto base = access.write ? command.destination : command.source;
    for (uint32_t lane = 0; lane < count; ++lane) {
        access.address[lane] = uint64_t{base} + transfer.offset + lane * transfer.width;
        for (uint32_t byte = 0; byte < transfer.width; ++byte)
            access.value[lane] |= uint32_t{transfer.data[lane * transfer.width + byte]} << (byte * 8);
    }
    return access;
}
}

// Aligned chunks use up to thirty-two words; unaligned chunks use up to thirty-two bytes.
// 对齐数据块最多使用三十二个字；非对齐数据块最多使用三十二个字节。
void Device::prepare_copy(Job& job) {
    const auto& c = job.command;
    auto& t = transfer_;
    const auto remaining = c.size - job.offset;
    const auto boundary = copy_backwards(c) ? remaining : job.offset;
    const bool aligned = (!reads_device(c.kind) || (c.source + boundary) % 4 == 0) &&
                         (!writes_device(c.kind) || (c.destination + boundary) % 4 == 0);
    t.width = aligned && remaining >= 4 ? 4 : 1;
    t.bytes = std::min(32 * t.width, remaining / t.width * t.width);
    t.offset = copy_backwards(c) ? remaining - t.bytes : job.offset;
    for (uint32_t byte = 0; byte < t.bytes; ++byte)
        t.data[byte] = c.kind == CommandKind::h2d ? c.data[t.offset + byte] : static_cast<uint8_t>(c.value);
    t.stage = reads_device(c.kind) ? CopyTransfer::Stage::read : CopyTransfer::Stage::write;
    if (c.kind == CommandKind::h2d) {
        t.stage = CopyTransfer::Stage::link_in;
        t.due = hardware_->cycle() + link_delay(t.bytes);
    }
    trace(job, "copy_chunk");
}

// DMA bypasses L1 reads while sharing L2, DRAM, and coherent write ordering with kernels.
// DMA 读取绕过 L1，同时与 kernel 共享 L2、DRAM 和一致的写入顺序。
void Device::submit_copy(Job& job) {
    auto& t = transfer_;
    t.ticket = hardware_->memory.submit(0, copy_access(job.command, t), t.owner, true);
    if (t.ticket) trace(job, t.stage == CopyTransfer::Stage::read ? "copy_read_submit" : "copy_write_submit");
}

// A chunk becomes host-visible only after memory acknowledgement and the outbound link delay.
// 数据块仅在内存确认和出站链路延迟结束后对主机可见。
void Device::poll_copy(Job& job) {
    auto& t = transfer_;
    const auto result = hardware_->memory.take(*t.ticket);
    if (!result) return;
    t.ticket.reset();
    trace(job, t.stage == CopyTransfer::Stage::read ? "copy_read_complete" : "copy_write_complete");
    if (t.stage == CopyTransfer::Stage::write) { complete_copy_chunk(job); return; }
    for (uint32_t byte = 0; byte < t.bytes; ++byte)
        t.data[byte] = static_cast<uint8_t>((*result)[byte / t.width] >> ((byte % t.width) * 8));
    t.stage = job.command.kind == CommandKind::d2d ? CopyTransfer::Stage::write : CopyTransfer::Stage::link_out;
    t.due = hardware_->cycle() + link_delay(t.bytes);
}

void Device::complete_copy_chunk(Job& job) {
    const auto& t = transfer_;
    if (job.command.kind == CommandKind::d2h)
        std::copy_n(t.data.begin(), t.bytes, job.result.begin() + t.offset);
    job.offset += t.bytes;
    transfer_.stage = CopyTransfer::Stage::prepare;
    if (job.offset == job.command.size) { copy_.reset(); finish(job); }
}

// Every pending ticket and link transfer owns the single staging slot until completion.
// 每个待完成 ticket 和链路传输持有唯一暂存槽，直至完成。
void Device::service_timed_copy(Job& job) {
    auto& t = transfer_;
    if (job.offset == job.command.size) { copy_.reset(); finish(job); return; }
    if (t.ticket) { poll_copy(job); return; }
    if (t.stage == CopyTransfer::Stage::prepare) { prepare_copy(job); return; }
    if (t.stage == CopyTransfer::Stage::link_in || t.stage == CopyTransfer::Stage::link_out) {
        if (hardware_->cycle() < t.due) return;
        if (t.stage == CopyTransfer::Stage::link_out) { complete_copy_chunk(job); return; }
        t.stage = CopyTransfer::Stage::write;
    }
    submit_copy(job);
}
}

#include "simt/hardware_atomic.hpp"
#include "simt/atomic.hpp"

namespace simt::hw {
// Read through L2 and latch the returned bytes before starting the configured integer ALU delay.
// 经 L2 读取并锁存返回字节，随后开始配置的整数 ALU 延迟。
bool GlobalAtomics::read(AtomicJob& job) {
    const auto& access = requests_.at(job.request).access;
    const auto address = access.address[job.lane];
    if (!job.memory) job.memory = l2_.submit(Packet{address / sector_bytes * sector_bytes});
    if (!job.memory) { ++stats_.read_stalls; return false; }
    const auto result = l2_.take(*job.memory);
    if (!result) return false;
    for (uint32_t byte = 0; byte < 4; ++byte)
        job.old |= uint32_t{(*result)[address % sector_bytes + byte]} << (byte * 8);
    job.value = atomic_result(*access.atomic, job.old, access.value[job.lane], access.replacement[job.lane]);
    job.due = cycle_ + config_.latency; job.stage = AtomicStage::execute; job.memory.reset();
    return false;
}

// Write only this lane's word and return its old value after the write acknowledgement.
// 仅写入当前 lane 的字，写确认后才返回旧值。
bool GlobalAtomics::write(AtomicJob& job) {
    auto& request = requests_.at(job.request);
    const auto address = request.access.address[job.lane], offset = address % sector_bytes;
    Packet packet{address - offset, true, {}, uint32_t{15} << offset};
    for (uint32_t byte = 0; byte < 4; ++byte) packet.data[offset + byte] = static_cast<uint8_t>(job.value >> (byte * 8));
    if (!job.memory) job.memory = l2_.submit(packet);
    if (!job.memory) { ++stats_.write_stalls; return false; }
    if (!l2_.take(*job.memory)) return false;
    request.result[job.lane] = job.old; --request.active; ++stats_.completed;
    return true;
}

bool GlobalAtomics::progress(AtomicJob& job) {
    if (job.stage == AtomicStage::read) return read(job);
    if (job.stage == AtomicStage::execute) {
        if (cycle_ < job.due) return false;
        job.stage = AtomicStage::write;
    }
    return write(job);
}
}

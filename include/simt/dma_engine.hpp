#pragma once
#include "simt/hardware_memory.hpp"
#include <memory>

namespace simt::hw {
struct DmaDescriptor {
    uint64_t id=0;
    uint32_t cta=0, source=0, destination=0, barrier=0, phase=0, bytes=0;
};
struct DmaCompletion {
    uint32_t slot=0;
    uint64_t id=0;
    uint32_t cta=0, barrier=0, phase=0, bytes=0;
};
struct DmaAccess {
    uint32_t slot=0;
    uint64_t id=0;
    uint32_t cta=0;
    WarpAccess access;
};
struct DmaReturn { uint64_t id=0; uint32_t bytes=0; };
struct DmaEngineStats {
    uint64_t clocks=0, active_clocks=0, submitted=0, released=0, fragments=0;
    uint64_t reads=0, writes=0, read_returns=0, write_returns=0, read_bytes=0, write_bytes=0;
    uint64_t completion_claims=0, fragment_stalls=0, read_stalls=0, write_stalls=0, completion_stalls=0;
    uint64_t peak_commands=0, peak_fragments=0;
};

// Payload and progress live in RTL; the external bridge only translates memory tickets and port handshakes.
// 载荷与进度保存在 RTL；外部桥接层只转换内存票据和端口握手。
class DmaEngine {
public:
    virtual ~DmaEngine() = default;
    virtual bool available() const = 0;
    virtual void submit(const DmaDescriptor& command) = 0;
    virtual std::optional<DmaAccess> request(bool write) const = 0;
    virtual void accept(bool write) = 0;
    virtual DmaReturn returned_read(uint32_t slot, const std::array<uint32_t,32>& data) = 0;
    virtual DmaReturn returned_write(uint32_t slot) = 0;
    virtual std::optional<DmaCompletion> completion() const = 0;
    virtual void claim() = 0;
    virtual void release(const DmaCompletion& completion) = 0;
    virtual uint32_t commands() const = 0;
    virtual uint32_t fragments() const = 0;
    virtual void tick() = 0;
    virtual const DmaEngineStats& stats() const = 0;
};
std::unique_ptr<DmaEngine> make_dma_engine(uint32_t commands, uint32_t fragments, uint32_t chunk);
}

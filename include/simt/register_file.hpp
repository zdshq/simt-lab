#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace simt::hw {
class OperandCollector;
using RegisterVector = std::array<uint32_t, 32>;
struct RegisterFileStats {
    uint64_t clocks = 0, active_clocks = 0, reads = 0, writes = 0, cleared_slots = 0;
};

// This interface owns actual register data; architectural dumps are observation copies only.
// 此接口持有实际寄存器数据；架构转储仅为观测副本。
class RegisterFile {
public:
    virtual ~RegisterFile() = default;
    virtual OperandCollector* collector() { return nullptr; }
    virtual void clear(uint32_t slot) = 0;
    virtual std::optional<RegisterVector> read(uint32_t slot, uint32_t reg) = 0;
    virtual bool writable(uint32_t reg) = 0;
    virtual void write(uint32_t slot, uint32_t reg, uint32_t mask, const RegisterVector& values) = 0;
    virtual void tick() = 0;
    virtual uint32_t port_groups() const = 0;
    virtual uint32_t slots() const = 0;
    virtual const RegisterFileStats& stats() const = 0;
};
bool register_rtl_available();
std::unique_ptr<RegisterFile> make_register_file(uint32_t partitions, uint32_t banks = 4, uint32_t collectors = 0);
std::unique_ptr<RegisterFile> make_rtl_register_file(uint32_t partitions, uint32_t banks);
}

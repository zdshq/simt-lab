#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace simt::hw {
struct AsyncConfig {
    uint32_t commands = 8, fragments = 4, chunk_bytes = 128;
    uint32_t barrier_slots = 16, barrier_latency = 2;
};
struct PipelineConfig {
    bool scoreboard_rtl = false, register_rtl = false;
    uint32_t partitions = 4, register_banks = 4, collectors = 4;
    uint32_t max_warps = 48, max_ctas = 24, register_words = 65536, shared_bytes = 65536;
    uint32_t integer_latency = 4, float_latency = 6, special_latency = 16;
    uint32_t integer_interval = 1, float_interval = 1, special_interval = 4;
};
struct RegisterUse { uint32_t reg, mask; };

// Writers block RAW/WAW hazards; readers retain WAR protection until physical operand collection.
// 写者阻塞 RAW/WAW 冒险，读者在物理操作数收集完成前保留 WAR 保护。
class Scoreboard {
public:
    bool readable(uint32_t reg, uint32_t mask) const;
    bool writable(uint32_t reg, uint32_t mask) const;
    bool predicate_ready(uint32_t pred, uint32_t mask) const;
    void reserve_read(uint32_t reg, uint32_t mask);
    void collect_read(uint32_t reg, uint32_t mask);
    void reserve_write(uint32_t reg, uint32_t mask);
    void retire_write(uint32_t reg, uint32_t mask);
    void reserve_predicate(uint32_t pred, uint32_t mask);
    void retire_predicate(uint32_t pred, uint32_t mask);
private:
    std::array<uint32_t, 64> writes_{};
    std::array<uint32_t, 8> predicates_{};
    std::array<std::array<uint32_t, 32>, 64> readers_{};
};

struct Residency {
    std::vector<uint32_t> slots;
    uint32_t registers = 0, shared = 0;
};

// Admission accounts rounded warp register allocation and shared-memory granularity per CTA.
// CTA 准入计入按 warp 取整的寄存器分配和 shared 分配粒度。
class SmResources {
public:
    explicit SmResources(PipelineConfig config = {});
    std::optional<Residency> acquire(uint32_t cta, uint32_t threads, uint32_t shared_bytes);
    void release(uint32_t cta);
    size_t ctas() const { return resident_.size(); }
    uint32_t used_registers() const { return registers_; }
    uint32_t used_shared() const { return shared_; }
private:
    PipelineConfig config_;
    std::vector<bool> slots_;
    std::map<uint32_t, Residency> resident_;
    uint32_t registers_ = 0, shared_ = 0;
};

// Independent read and write bank ports reset only on a new model clock.
// 独立的读写 bank 端口只在新的模型时钟重置。
class RegisterPorts {
public:
    explicit RegisterPorts(uint32_t banks);
    bool read(uint32_t reg, uint64_t cycle);
    bool write(uint32_t reg, uint64_t cycle);
private:
    std::vector<std::optional<uint64_t>> read_, write_;
    bool claim(std::vector<std::optional<uint64_t>>& ports, uint32_t reg, uint64_t cycle);
};
}

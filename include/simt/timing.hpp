#pragma once
#include "simt/machine.hpp"
#include "simt/hardware_memory.hpp"
#include <deque>

namespace simt {
enum class Pipe { integer, floating, special, memory, control };
enum class Stage { collect, ready, executing, memory, writeback };
struct Footprint {
    Pipe pipe = Pipe::integer;
    std::vector<hw::RegisterUse> reads;
    std::optional<uint32_t> destination, predicate;
    std::optional<uint32_t> source_predicate;
    uint32_t write_mask = 0;
    bool boundary = false, store = false;
};
Footprint instruction_footprint(const Instruction& instruction, uint32_t mask);
std::string timing_location(uint32_t cta, uint32_t sm, uint32_t warp, uint32_t pc);

struct TimedWarp {
    hw::Scoreboard scoreboard;
    uint32_t inflight = 0, slot = 0;
    bool boundary = false, memory = false;
};
struct WarpOwner { uint32_t cta, warp; };
struct Partition {
    hw::RegisterPorts ports;
    uint32_t cursor = 0;
    std::array<uint64_t, 5> available{};
    std::optional<uint64_t> predicate_write;
    explicit Partition(uint32_t banks) : ports(banks) {}
};
struct TimedSm {
    hw::SmResources resources;
    std::vector<Partition> partitions;
    std::vector<std::optional<WarpOwner>> owners;
    hw::BankedShared shared;
    hw::BankedShared constant{{65536, 1, 4, 8}};
    explicit TimedSm(const hw::PipelineConfig& config);
};
struct MemoryFlight {
    uint32_t space = 0;
    hw::WarpAccess access;
    std::optional<hw::Ticket> ticket;
    bool done = false;
};

// Collected operands and pending results remain separate from committed register state.
// 已收集操作数和待写回结果与已提交寄存器状态保持分离。
struct Inflight {
    uint64_t id = 0, due = 0;
    uint32_t cta = 0, warp = 0, sm = 0, partition = 0, mask = 0;
    Group group{};
    Instruction instruction{};
    Footprint footprint;
    Stage stage = Stage::collect;
    Warp operands, result;
    std::vector<hw::RegisterUse> unread;
    std::vector<MemoryFlight> memory;
};
struct TimingStats {
    uint64_t issued = 0, retired = 0, scoreboard_stalls = 0, boundary_stalls = 0;
    uint64_t collector_stalls = 0, register_stalls = 0, unit_stalls = 0, writeback_stalls = 0;
    uint64_t memory_order_stalls = 0, peak_ctas = 0, dropped_trace = 0;
};
struct HardwareEvent {
    uint64_t cycle, instruction;
    uint32_t sm, cta, warp, pc;
    std::string action;
};

// The timing machine drives the same architectural state through finite hardware resources.
// 时序模拟机通过有限硬件资源驱动同一套架构状态。
class TimingMachine {
public:
    explicit TimingMachine(Machine& machine);
    bool advance();
    void dump(std::ostream& out) const;
private:
    Machine& machine_;
    std::shared_ptr<hw::MemoryHierarchy> memory_;
    std::vector<std::unique_ptr<TimedSm>> sms_;
    std::vector<std::vector<TimedWarp>> warps_;
    std::map<uint64_t, std::unique_ptr<Inflight>> inflight_;
    std::deque<HardwareEvent> trace_;
    TimingStats stats_;
    uint64_t cycle_ = 0, next_ = 1;
    bool admit();
    void place(uint32_t block, uint32_t sm, const hw::Residency& residency);
    void retire_ctas();
    bool issue_partition(uint32_t sm, uint32_t partition);
    bool try_issue(uint32_t cta, uint32_t warp);
    bool issue_group(uint32_t cta, uint32_t warp, Group group);
    bool dependencies(uint32_t cta, uint32_t warp, Group group, const Footprint& footprint) const;
    bool capacity(uint32_t sm, uint32_t partition);
    void enqueue(uint32_t cta, uint32_t warp, Group group, uint32_t mask, Footprint footprint);
    void reserve(Inflight& entry);
    void collect_operands();
    void launch_ready();
    void launch(Inflight& entry);
    void evaluate_scalar(Inflight& entry);
    void commit_control(Inflight& entry);
    void poll_memory(Inflight& entry);
    void prepare_memory(Inflight& entry);
    void finish_memory(Inflight& entry, const MemoryFlight& flight, const std::array<uint32_t, 32>& values);
    void retire_results();
    bool writeback_available(Inflight& entry);
    void commit(Inflight& entry);
    void trace(const Inflight& entry, const std::string& action);
    void dump_statistics(std::ostream& out) const;
    void dump_memory(std::ostream& out) const;
    void dump_trace(std::ostream& out) const;
};
}

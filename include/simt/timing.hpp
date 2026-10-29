#pragma once
#include "simt/machine.hpp"
#include "simt/hardware_memory.hpp"
#include "simt/hardware_context.hpp"
#include "simt/timing_tensor.hpp"
#include "simt/timing_async.hpp"
#include "simt/scoreboard_engine.hpp"
#include "simt/register_file.hpp"
#include "simt/operand_collector.hpp"
#include <deque>

namespace simt {
enum class Pipe { integer, floating, special, memory, control, tensor, async };
enum class Stage { collect, ready, executing, memory, tensor, async, writeback };
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
    hw::ScoreboardEngine* scoreboard = nullptr;
    uint32_t inflight = 0, slot = 0;
    bool boundary = false, memory = false;
};
struct WarpOwner { uint32_t cta, warp; };
struct Partition {
    hw::RegisterPorts ports;
    std::unique_ptr<hw::RegisterFile> registers;
    std::array<std::optional<uint64_t>,32> collector_owners;
    uint32_t cursor = 0;
    std::array<uint64_t, 7> available{};
    std::optional<uint64_t> predicate_write;
    explicit Partition(uint32_t banks) : ports(banks) {}
};
struct TimedSm {
    hw::SmResources resources;
    std::vector<Partition> partitions;
    std::vector<std::optional<WarpOwner>> owners;
    std::vector<std::unique_ptr<hw::ScoreboardEngine>> scoreboards;
    hw::BankedShared shared;
    hw::BankedShared constant{{65536, 1, 4, 8}};
    hw::BankedShared tmem;
    std::unique_ptr<hw::TensorEngine> array;
    std::optional<uint64_t> tensor_owner;
    std::map<uint32_t, uint32_t> tensor_columns;
    uint32_t allocated_columns = 0;
    std::deque<DmaCommand> copies;
    std::optional<BarrierJob> barrier_job;
    AsyncStats async_stats;
    uint32_t copy_cursor = 0, copy_fragments = 0, barrier_entries = 0;
    explicit TimedSm(const hw::PipelineConfig& config, const hw::TensorConfig& tensor, const hw::AtomicConfig& atomic);
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
    std::optional<uint32_t> collector;
    std::vector<MemoryFlight> memory;
    std::optional<TensorFlight> tensor;
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
    explicit TimingMachine(Machine& machine, std::shared_ptr<hw::HardwareContext> hardware = {});
    ~TimingMachine();
    bool advance();
    bool drain();
    void dump(std::ostream& out) const;
private:
    Machine& machine_;
    std::shared_ptr<hw::HardwareContext> hardware_;
    hw::MemoryHierarchy* memory_ = nullptr;
    bool external_clock_ = false;
    uint64_t owner_ = 0, local_base_ = 0, started_ = 0;
    std::vector<std::unique_ptr<TimedSm>> sms_;
    std::vector<std::vector<TimedWarp>> warps_;
    std::map<uint64_t, std::unique_ptr<Inflight>> inflight_;
    std::deque<HardwareEvent> trace_;
    TimingStats stats_;
    uint64_t cycle_ = 0, next_ = 1, next_copy_ = 1;
    std::optional<hw::Ticket> submit_memory(uint32_t sm, const hw::WarpAccess& access);
    bool admit();
    void place(uint32_t block, uint32_t sm, const hw::Residency& residency);
    bool cta_drained(const Cta& cta) const;
    void retire_ctas();
    bool issue_partition(uint32_t sm, uint32_t partition);
    bool try_issue(uint32_t cta, uint32_t warp);
    bool issue_group(uint32_t cta, uint32_t warp, Group group);
    bool dependencies(uint32_t cta, uint32_t warp, Group group, const Footprint& footprint) const;
    bool capacity(uint32_t sm, uint32_t partition);
    void enqueue(uint32_t cta, uint32_t warp, Group group, uint32_t mask, Footprint footprint);
    void reserve(Inflight& entry);
    void collect_operands();
    void collect_rtl_operands();
    void dispatch_collector_operands();
    bool async_dispatch_eligible(const Inflight& entry) const;
    void reserve_collector(Inflight& entry);
    void take_collector(Inflight& entry);
    bool collect_operand(Inflight& entry, const hw::RegisterUse& use);
    void commit_register_data(Inflight& entry);
    void launch_ready();
    void launch(Inflight& entry);
    void evaluate_scalar(Inflight& entry);
    void commit_control(Inflight& entry);
    void poll_memory(Inflight& entry);
    void prepare_memory(Inflight& entry);
    void prepare_atomic(Inflight& entry);
    void finish_memory(Inflight& entry, const MemoryFlight& flight, const std::array<uint32_t, 32>& values);
    void poll_tensor(Inflight& entry);
    void advance_tensor(Inflight& entry);
    void start_tensor(Inflight& entry);
    void start_tensor_transfer(Inflight& entry);
    void start_tensor_descriptor(Inflight& entry, uint32_t address);
    void free_tensor(Inflight& entry, uint32_t address);
    void allocate_tensor(Inflight& entry);
    void zero_tensor(Inflight& entry);
    bool poll_tensor_transfers(Inflight& entry);
    bool service_tensor_transfer(Inflight& entry, TensorTransfer& part);
    void tensor_descriptor(Inflight& entry);
    void begin_tensor_tile(Inflight& entry);
    void initial_tensor_tile(Inflight& entry);
    void prepare_tensor_wave(Inflight& entry);
    void tensor_metadata(Inflight& entry);
    void prepare_tensor_operands(Inflight& entry);
    void prepare_tensor_scales(Inflight& entry);
    void decode_tensor_operands(Inflight& entry);
    void feed_tensor(Inflight& entry);
    void drain_tensor(Inflight& entry);
    void store_tensor(Inflight& entry);
    bool async_available(const Inflight& entry);
    void start_async(Inflight& entry);
    void reserve_barrier_record(const Inflight& entry, uint32_t address);
    void check_pending_shared_writes(uint32_t cta, uint32_t address, uint32_t bytes) const;
    void progress_async_units();
    void progress_barrier(uint32_t sm);
    void prepare_barrier_job(uint32_t sm, BarrierJob& job);
    void prepare_copy_issue(BarrierJob& job, const Inflight& entry);
    void publish_barrier_job(uint32_t sm, BarrierJob& job);
    void finish_barrier_job(uint32_t sm, BarrierJob& job);
    void query_async_wait(uint32_t sm, BarrierJob& job);
    void commit_async_instruction(Inflight& entry);
    void submit_async_copy(uint32_t sm, const BarrierJob& job);
    void service_copy_fragments(uint32_t sm);
    bool service_copy_fragment(DmaCommand& copy, DmaFragment& fragment);
    void admit_copy_fragment(uint32_t sm);
    void complete_ready_copy(uint32_t sm);
    void finish_copy(uint32_t sm, uint64_t copy);
    DmaCommand& find_copy(uint32_t sm, uint64_t copy);
    bool pending_copy(uint32_t cta) const;
    bool background_work() const;
    void trace_copy(const DmaCommand& copy, const std::string& action);
    void retire_results();
    bool writeback_available(Inflight& entry);
    void commit(Inflight& entry);
    void trace(const Inflight& entry, const std::string& action);
    void dump_statistics(std::ostream& out) const;
    void dump_memory(std::ostream& out) const;
    void dump_tensor(std::ostream& out) const;
    void dump_async(std::ostream& out) const;
    void dump_atomic(std::ostream& out) const;
    void dump_trace(std::ostream& out) const;
};
}

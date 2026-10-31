#pragma once
#include "simt/machine.hpp"
#include "simt/monitor.hpp"
#include "simt/hardware_context.hpp"
#include "simt/command_processor.hpp"
#include <deque>
#include <functional>
#include <set>

namespace simt {
using JobId = uint64_t;
enum class CommandKind { h2d, d2h, d2d, fill, kernel, empty, graph };
enum class JobState { queued, waiting, running, complete, error, cancelled };
struct Command {
    CommandKind kind = CommandKind::empty;
    uint32_t destination = 0, source = 0, size = 0, value = 0;
    uint64_t module = 0, graph = 0;
    Config launch;
    std::vector<uint32_t> arguments, buffers;
    std::vector<uint8_t> data;
};
struct GraphNode { Command command; std::vector<uint32_t> dependencies; };
struct GraphExecutable {
    std::vector<GraphNode> nodes;
    uint64_t in_flight = 0;
    JobId last_launch = 0;
};
struct Allocation { uint32_t size = 0; uint64_t pins = 0; };

// The arena owns allocation lifetimes while kernel machines share the backing memory.
// arena 管理分配生命周期，kernel 模拟机共享底层内存。
class DeviceArena {
public:
    std::shared_ptr<Memory> memory = std::make_shared<Memory>();
    std::function<void(uint64_t, uint64_t)> before_clear;
    uint32_t allocate(uint32_t size);
    void release(uint32_t address);
    uint32_t containing(uint32_t address, uint32_t size) const;
    void validate(uint32_t address) const;
    void pin(const std::set<uint32_t>& addresses);
    void unpin(const std::set<uint32_t>& addresses);
    uint64_t allocated_bytes() const;
private:
    std::map<uint32_t, Allocation> allocations_;
};

// A fetched command remains distinct from a completed job and retains its resource pins.
// 已取走命令与已完成任务不同，任务仍持有资源引用。
struct Job {
    JobId id = 0, parent = 0;
    uint32_t stream = 0, node = 0, offset = 0, expanded = 0;
    JobState state = JobState::queued;
    Command command;
    std::vector<JobId> dependencies, children;
    std::set<uint32_t> pinned;
    std::unique_ptr<Machine> machine;
    std::vector<uint8_t> result;
    std::string error;
    std::string hardware = "null";
    bool recovering = false;
    uint64_t started = 0, finished = 0;
    uint64_t instruction_steps = 0;
    uint32_t cp_slot = 0;
    std::vector<JobId> external;
};
struct TraceEntry { uint64_t tick; JobId job, parent; uint32_t node; std::string action; };

// The copy engine retains one bounded chunk while the link or memory hierarchy applies backpressure.
// 链路或内存层次施加反压时，拷贝引擎保留一个有界数据块。
struct CopyTransfer {
    enum class Stage { prepare, link_in, read, write, link_out };
    Stage stage = Stage::prepare;
    uint64_t owner = 0, due = 0;
    uint32_t offset = 0, bytes = 0, width = 1;
    std::array<uint8_t, 128> data{};
    std::optional<hw::Ticket> ticket;
};

// A single compute engine and a copy engine make progress through explicit host polling.
// 一个计算引擎与一个拷贝引擎通过显式主机轮询推进。
class Device {
public:
    explicit Device(uint32_t sms = 2, uint32_t capacity = 64, bool timing = false,
                    bool hardware_trace = false, bool tensor_rtl = false, bool cache_rtl = false, bool scoreboard_rtl = false, bool register_rtl = false, bool collector_rtl = false);
    DeviceArena arena;
    uint64_t load_module(const std::vector<uint8_t>& bytes);
    JobId submit(uint32_t stream, Command command, std::vector<JobId> dependencies = {});
    void doorbell();
    void tick();
    void wait(JobId id, uint64_t limit);
    void synchronize(uint64_t limit);
    const Job& query(JobId id) const;
    uint64_t instantiate(std::vector<GraphNode> nodes);
    void update_graph(uint64_t graph, uint32_t node, Command command);
    std::string job_json(JobId id) const;
    std::string stats_json() const;
    std::string trace_json() const;
    void publish_monitor();
private:
    uint32_t sms_;
    bool hardware_trace_, tensor_rtl_, cache_rtl_, scoreboard_rtl_, register_rtl_, collector_rtl_;
    std::shared_ptr<hw::HardwareContext> hardware_;
    uint64_t written_ = 0, fetched_ = 0, published_ = 0, ticks_ = 0, notifications_ = 0;
    JobId next_job_ = 1;
    uint64_t next_module_ = 1, next_graph_ = 1, dropped_trace_ = 0;
    std::vector<JobId> ring_;
    std::map<uint64_t, Module> modules_;
    std::map<JobId, Job> jobs_;
    std::map<uint32_t, JobId> stream_tail_;
    std::map<uint64_t, GraphExecutable> graphs_;
    std::deque<TraceEntry> trace_;
    std::optional<JobId> compute_, copy_;
    CopyTransfer transfer_;
    std::unique_ptr<MonitorPublisher> monitor_;
    std::unique_ptr<hw::CommandProcessor> cp_;
    std::array<JobId, 16> cp_jobs_{};
    uint64_t fetched_count() const;
    uint64_t published_count() const;
    uint64_t notified_count() const;
    void release_compute();
    void release_copy();
    void fetch_circuit();
    void dispatch_circuit();
    void offer_external_dependencies();
    std::optional<hw::CpNode> oldest_node() const;
    void expand_circuit_graph();
    void validate(const Command& command) const;
    std::set<uint32_t> references(const Command& command) const;
    void fetch();
    void dispatch();
    void start(Job& job);
    void start_kernel(Job& job);
    void start_copy(Job& job);
    void finish(Job& job, JobState state = JobState::complete, std::string error = {});
    void service_compute();
    void service_copy();
    void service_timed_copy(Job& job);
    void prepare_copy(Job& job);
    void submit_copy(Job& job);
    void poll_copy(Job& job);
    void complete_copy_chunk(Job& job);
    void expand_graph(Job& job);
    void service_graphs();
    void expand_node(Job& parent);
    void finish_graphs();
    void trace(const Job& job, const std::string& action);
    int readiness(const Job& job) const;
};
bool terminal(JobState state);
const char* state_name(JobState state);
std::string json_string(const std::string& value);
std::string encode_hex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> decode_hex(const std::string& text);
}

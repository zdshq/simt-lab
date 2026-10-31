#include "simt/device.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt {
namespace {
// Kahn's traversal validates the entire graph, including disconnected components.
// Kahn 遍历检查整张图，包括不连通分量。
void check_dag(const std::vector<GraphNode>& nodes) {
    std::vector<uint32_t> degree(nodes.size());
    std::vector<std::vector<uint32_t>> outgoing(nodes.size());
    std::vector<uint32_t> ready;
    for (uint32_t node = 0; node < nodes.size(); ++node) {
        degree[node] = static_cast<uint32_t>(nodes[node].dependencies.size());
        for (auto dependency : nodes[node].dependencies) {
            if (dependency >= nodes.size()) throw std::runtime_error("invalid_graph_dependency");
            outgoing[dependency].push_back(node);
        }
        if (!degree[node]) ready.push_back(node);
    }
    for (size_t at = 0; at < ready.size(); ++at)
        for (auto child : outgoing[ready[at]])
            if (!--degree[child]) ready.push_back(child);
    if (ready.size() != nodes.size()) throw std::runtime_error("graph_cycle");
}
}

// Instantiation snapshots validated commands without executing or submitting any work.
// 实例化保存经过验证的命令快照，不执行或提交任何工作。
uint64_t Device::instantiate(std::vector<GraphNode> nodes) {
    if (nodes.size() > 512) throw std::runtime_error("graph_too_large");
    for (auto& node : nodes) {
        if (node.command.kind == CommandKind::graph) throw std::runtime_error("nested_graph_unsupported");
        validate(node.command);
        auto& deps = node.dependencies;
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }
    check_dag(nodes);
    const auto id = next_graph_++;
    graphs_.emplace(id, GraphExecutable{std::move(nodes), 0, 0});
    return id;
}

// Parameter updates preserve topology and are forbidden while any replay is outstanding.
// 参数更新保留拓扑，并禁止在任何重放尚未完成时修改。
void Device::update_graph(uint64_t id, uint32_t node, Command command) {
    if (!graphs_.contains(id)) throw std::runtime_error("invalid_graph");
    auto& graph = graphs_.at(id);
    if (graph.in_flight) throw std::runtime_error("graph_in_use");
    if (node >= graph.nodes.size()) throw std::runtime_error("invalid_graph_node");
    if (graph.nodes[node].command.kind != command.kind) throw std::runtime_error("graph_node_type_change");
    validate(command);
    graph.nodes[node].command = std::move(command);
}

// One queue packet expands to internal nodes whose edges depend on actual completion.
// 一个队列包展开为内部节点，其依赖边等待实际完成。
void Device::expand_graph(Job& parent) {
    const auto& nodes = graphs_.at(parent.command.graph).nodes;
    for (uint32_t index = 0; index < nodes.size(); ++index) {
        Job job;
        job.id = next_job_++; job.parent = parent.id; job.node = index;
        parent.children.push_back(job.id);
        const auto id = job.id;
        jobs_.emplace(id, std::move(job));
    }
    if (!hardware_) while (parent.expanded < nodes.size()) expand_node(parent);
}

// Reserving all handles first permits dependencies on nodes decoded in later clocks.
// 先预约全部句柄，允许依赖在后续时钟才解码的节点。
void Device::expand_node(Job& parent) {
    const auto index = parent.expanded++;
    const auto& node = graphs_.at(parent.command.graph).nodes[index];
    auto& job = jobs_.at(parent.children[index]);
    job.stream = parent.stream; job.state = JobState::waiting;
    job.command = node.command; job.dependencies = parent.dependencies;
    for (auto dependency : node.dependencies) job.dependencies.push_back(parent.children[dependency]);
    trace(job, "graph_node");
}

// One shared graph decoder materializes at most one node per device clock.
// 一个共享图解码器每个设备时钟最多展开一个节点。
void Device::service_graphs() {
    if (!hardware_) return;
    if (cp_) { expand_circuit_graph(); return; }
    for (auto& [id, job] : jobs_) {
        static_cast<void>(id);
        if (job.command.kind != CommandKind::graph || job.state != JobState::running) continue;
        if (job.expanded == job.children.size()) continue;
        expand_node(job);
        return;
    }
}

// Graph completion waits for all branches, including branches independent of an error.
// 图完成等待所有分支，包括不受某个错误影响的独立分支。
void Device::finish_graphs() {
    for (auto& [id, job] : jobs_) {
        static_cast<void>(id);
        if (job.command.kind != CommandKind::graph || job.state != JobState::running) continue;
        if (job.expanded != job.children.size()) continue;
        bool complete = true;
        std::string error;
        for (auto child : job.children) {
            const auto& node = query(child);
            complete &= terminal(node.state);
            if (terminal(node.state) && node.state != JobState::complete && error.empty())
                error = "graph_failed node=" + std::to_string(node.node) + ": " + node.error;
        }
        if (complete) finish(job, error.empty() ? JobState::complete : JobState::error, error);
    }
}
}

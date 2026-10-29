#include "simt/protocol.hpp"
#include <stdexcept>

namespace simt {
namespace {
// Allocate, free, and decode resources without implicitly advancing execution.
// 分配、释放和解码资源，不隐式推进执行。
std::string resource_request(Device& device, RequestReader& reader, const std::string& operation) {
    if (operation == "LOAD") {
        const auto data = decode_hex(reader.token()); reader.end();
        return std::to_string(device.load_module(data));
    }
    const auto value = reader.word(); reader.end();
    if (operation == "ALLOC") return std::to_string(device.arena.allocate(value));
    device.arena.release(value);
    return "null";
}

// Validate the complete submission packet before consuming a queue slot.
// 消耗队列位置之前验证完整提交包。
std::string submit_request(Device& device, RequestReader& reader) {
    const auto stream = reader.word();
    const auto count = reader.count(4096);
    std::vector<JobId> dependencies;
    for (uint32_t index = 0; index < count; ++index) dependencies.push_back(reader.number());
    auto command = read_command(reader); reader.end();
    return std::to_string(device.submit(stream, std::move(command), std::move(dependencies)));
}

// A graph request contains all nodes, enabling atomic validation and instantiation.
// 图请求包含全部节点，可原子地验证并实例化。
std::string graph_request(Device& device, RequestReader& reader) {
    const auto count = reader.count(512);
    std::vector<GraphNode> nodes;
    for (uint32_t index = 0; index < count; ++index) {
        GraphNode node;
        const auto dependencies = reader.count(512);
        for (uint32_t dep = 0; dep < dependencies; ++dep) node.dependencies.push_back(reader.word());
        node.command = read_command(reader);
        nodes.push_back(std::move(node));
    }
    reader.end();
    return std::to_string(device.instantiate(std::move(nodes)));
}

// A node update changes operands only after a complete request has passed parsing.
// 节点更新仅在完整请求解析通过后改变操作数。
std::string update_request(Device& device, RequestReader& reader) {
    const auto graph = reader.number();
    const auto node = reader.word();
    auto command = read_command(reader); reader.end();
    device.update_graph(graph, node, std::move(command));
    return "null";
}

// Explicit progress keeps tests reproducible and gives bounded waits a precise meaning.
// 显式推进使测试可复现，并为有界等待提供明确含义。
std::string progress_request(Device& device, RequestReader& reader, const std::string& operation) {
    if (operation == "WAIT") {
        const auto job = reader.number(), limit = reader.number(); reader.end();
        if (limit > 10000000) throw std::runtime_error("progress_limit_out_of_range");
        device.wait(job, limit); return device.job_json(job);
    }
    const auto count = reader.number(); reader.end();
    if (count > 10000000) throw std::runtime_error("progress_limit_out_of_range");
    if (operation == "DRAIN") device.synchronize(count);
    else for (uint64_t index = 0; index < count; ++index) device.tick();
    return device.stats_json();
}
}

// Route requests to small parsers; every response is one complete JSON value.
// 将请求分发给短解析函数，每个响应都是完整 JSON 值。
std::string execute_request(Device& device, const std::string& line) {
    RequestReader reader(line);
    const auto operation = reader.token();
    if (operation == "ALLOC" || operation == "FREE" || operation == "LOAD") return resource_request(device, reader, operation);
    if (operation == "SUBMIT") return submit_request(device, reader);
    if (operation == "GRAPH") return graph_request(device, reader);
    if (operation == "UPDATE") return update_request(device, reader);
    if (operation == "TICK" || operation == "WAIT" || operation == "DRAIN") return progress_request(device, reader, operation);
    if (operation == "QUERY") { const auto id = reader.number(); reader.end(); return device.job_json(id); }
    reader.end();
    if (operation == "BELL") { device.doorbell(); return "null"; }
    if (operation == "STATS" || operation == "HELLO") return device.stats_json();
    if (operation == "TRACE") return device.trace_json();
    throw std::runtime_error("unknown_protocol_operation");
}
}

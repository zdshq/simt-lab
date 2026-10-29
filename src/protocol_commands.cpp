#include "simt/protocol.hpp"
#include <stdexcept>

namespace simt {
namespace {
// Kernel launch records include explicit buffer references for lifetime tracking.
// kernel 启动记录包含显式缓冲区引用，用于生命周期跟踪。
Command kernel_command(RequestReader& reader) {
    Command c; c.kind = CommandKind::kernel;
    c.module = reader.number(); c.launch.blocks = reader.word(); c.launch.threads = reader.word();
    c.launch.max_steps = reader.number();
    const auto arguments = reader.count(16384);
    for (uint32_t index = 0; index < arguments; ++index) c.arguments.push_back(reader.word());
    const auto buffers = reader.count(16384);
    for (uint32_t index = 0; index < buffers; ++index) c.buffers.push_back(reader.word());
    return c;
}
}

// Decode an operation independently of its queue or graph container.
// 独立于队列或图容器解码操作。
Command read_command(RequestReader& reader) {
    const auto kind = reader.token();
    Command c;
    if (kind == "KERNEL") return kernel_command(reader);
    if (kind == "EMPTY") return c;
    if (kind == "GRAPH") { c.kind = CommandKind::graph; c.graph = reader.number(); return c; }
    if (kind == "H2D") {
        c.kind = CommandKind::h2d; c.destination = reader.word(); c.data = decode_hex(reader.token());
        c.size = static_cast<uint32_t>(c.data.size()); return c;
    }
    if (kind == "D2H") { c.kind = CommandKind::d2h; c.source = reader.word(); c.size = reader.word(); return c; }
    if (kind == "D2D") {
        c.kind = CommandKind::d2d; c.destination = reader.word(); c.source = reader.word(); c.size = reader.word();
        return c;
    }
    if (kind == "FILL") {
        c.kind = CommandKind::fill; c.destination = reader.word(); c.size = reader.word(); c.value = reader.word();
        return c;
    }
    throw std::runtime_error("unknown_command_kind");
}
}

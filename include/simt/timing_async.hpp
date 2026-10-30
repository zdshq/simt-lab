#pragma once
#include "simt/hardware_memory.hpp"
#include "simt/machine.hpp"

namespace simt {
enum class FragmentStage { read, write };
struct DmaFragment {
    uint32_t offset, bytes;
    hw::WarpAccess access;
    std::optional<hw::Ticket> ticket;
    FragmentStage stage = FragmentStage::read;
};
struct DmaCommand {
    uint64_t id, instruction;
    uint32_t cta, sm, warp, pc, source, destination, address, phase, bytes;
    uint32_t cursor = 0;
    std::deque<DmaFragment> fragments;
};
enum class BarrierAction { initialize, expect, arrive, issue, wait, complete };
struct BarrierJob {
    BarrierAction action;
    uint64_t instruction = 0, copy = 0, due = 0;
    uint32_t cta = 0, address = 0, remaining = 0;
    TransactionBarrier next{};
    std::optional<hw::Ticket> ticket{};
    bool prepared = false, phase_advanced = false;
    bool submitted = false, publication_done = false;
};
struct AsyncStats {
    uint64_t copies = 0, completed = 0, read_bytes = 0, write_bytes = 0, fragments = 0;
    uint64_t command_stalls = 0, fragment_stalls = 0, memory_stalls = 0, peak_commands = 0;
    uint64_t barrier_requests = 0, barrier_stalls = 0, wait_queries = 0, phase_completions = 0;
};
}

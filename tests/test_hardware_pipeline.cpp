#include "hardware_support.hpp"
#include "simt/hardware_pipeline.hpp"

namespace simt::hw::test {
// Disjoint lane masks can progress while overlapping RAW/WAW dependencies remain blocked.
// 不相交的 lane 掩码可继续执行，重叠的 RAW/WAW 依赖仍需阻塞。
void masked_writers() {
    Scoreboard board;
    board.reserve_write(3, 0xFF);
    require(!board.readable(3, 1) && !board.writable(3, 1), "RAW/WAW escaped");
    require(board.readable(3, 0x100) && board.writable(3, 0x100), "disjoint lanes blocked");
    board.reserve_write(3, 0x100);
    rejects([&] { board.reserve_write(3, 1); }, "duplicate_writer");
    board.retire_write(3, 0xFF);
    require(board.readable(3, 0xFF) && !board.readable(3, 0x100), "retired wrong lanes");
    board.retire_write(3, 0x100);
    require(board.writable(3, ~0U), "writer reservation leaked");
}

// Multiple collectors retain the old value until the last physical read completes.
// 多个收集器在最后一次物理读取结束前共同保护旧值。
void reader_lifetime() {
    Scoreboard board;
    board.reserve_read(4, 3); board.reserve_read(4, 1);
    require(board.readable(4, 3) && !board.writable(4, 3), "WAR was not held");
    board.collect_read(4, 3);
    require(board.writable(4, 2) && !board.writable(4, 1), "reader count lost");
    board.collect_read(4, 1);
    require(board.writable(4, 3), "reader reservation leaked");
    rejects([&] { board.collect_read(4, 1); }, "unowned_read");
}

void read_modify_write() {
    Scoreboard board;
    board.reserve_read(5, 1); board.reserve_write(5, 1);
    require(!board.readable(5, 1), "RMW destination was visible early");
    board.collect_read(5, 1); board.retire_write(5, 1);
    require(board.writable(5, 1), "RMW did not release reservations");
    board.reserve_write(63, ~0U); board.reserve_read(63, ~0U);
    require(board.readable(63, ~0U) && board.writable(63, ~0U), "zero register blocked");
}

void predicate_writers() {
    Scoreboard board;
    board.reserve_predicate(2, 7);
    require(!board.predicate_ready(2, 1) && board.predicate_ready(2, 8), "predicate mask lost");
    rejects([&] { board.reserve_predicate(2, 1); }, "predicate_conflict");
    require(board.predicate_ready(7, ~0U), "constant predicate blocked");
    board.retire_predicate(2, 7);
    require(board.predicate_ready(2, ~0U), "predicate reservation leaked");
}

// A partial warp consumes a full register allocation; failed admission changes no ownership.
// 不足一个 warp 的线程仍占完整寄存器分配，准入失败不改变资源归属。
void resource_registers() {
    PipelineConfig config; config.register_words = 6144;
    SmResources resources(config);
    const auto first = resources.acquire(10, 33, 129);
    require(first && first->registers == 4096 && first->shared == 256, "allocation rounding");
    require(!resources.acquire(11, 33, 0), "register exhaustion ignored");
    require(resources.ctas() == 1 && resources.used_registers() == 4096, "failed admission leaked");
    const auto second = resources.acquire(11, 1, 0);
    require(second && second->slots == std::vector<uint32_t>{2}, "partial-warp slot allocation");
    resources.release(10);
    const auto reused = resources.acquire(12, 64, 0);
    require(reused && reused->slots == std::vector<uint32_t>({0, 1}), "slots were not reused");
    resources.release(11); resources.release(12);
    require(!resources.ctas() && !resources.used_registers() && !resources.used_shared(), "occupancy leaked");
}

void resource_shared_and_slots() {
    PipelineConfig config; config.max_warps = 3; config.max_ctas = 2; config.shared_bytes = 512;
    SmResources resources(config);
    require(resources.acquire(0, 64, 300).has_value(), "initial admission failed");
    require(!resources.acquire(1, 32, 129), "rounded shared overflow accepted");
    require(!resources.acquire(1, 64, 0), "warp overflow accepted");
    require(resources.acquire(1, 32, 128).has_value(), "exact shared capacity rejected");
    require(!resources.acquire(2, 1, 0), "CTA capacity exceeded");
    resources.release(0); resources.release(1);
    rejects([&] { resources.release(1); }, "unknown_resident_cta");
}

// Read and write ports are independent, but each bank has only one of each per clock.
// 读写端口相互独立，但每个 bank 每拍各只有一个。
void register_bank_ports() {
    RegisterPorts ports(4);
    require(ports.read(0, 0) && !ports.read(4, 0), "read bank conflict ignored");
    require(ports.read(1, 0) && ports.write(0, 0), "independent ports blocked");
    require(!ports.write(8, 0), "write bank conflict ignored");
    require(ports.read(4, 1) && ports.write(8, 1), "ports failed to reset");
    require(ports.read(63, 1) && ports.read(63, 1), "zero register used a port");
    rejects([] { RegisterPorts invalid(0); }, "invalid_register_banks");
}

int run_tests() {
    try {
        run("masked writers", masked_writers);
        run("reader lifetime", reader_lifetime);
        run("read modify write", read_modify_write);
        run("predicate writers", predicate_writers);
        run("register admission and reuse", resource_registers);
        run("shared and warp admission", resource_shared_and_slots);
        run("register bank ports", register_bank_ports);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return simt::hw::test::run_tests(); }

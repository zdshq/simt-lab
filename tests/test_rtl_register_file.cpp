#include "simt/register_file.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using simt::hw::RegisterVector;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, "invalid register port operation accepted");
}

// Each partition geometry must retain the last slot, all lane bits, and the full packed row address.
// 每种分区几何都必须保留末尾槽位、全部 lane 位和完整打包行地址。
void geometries_and_port_capacity() {
    for (uint32_t partitions : {1, 2, 4, 8}) {
        auto file = simt::hw::make_register_file(partitions);
        require(file->slots()*partitions == 32, "SM storage capacity changed");
        RegisterVector values;
        for (uint32_t lane = 0; lane < 32; ++lane) values[lane] = 0x81234500u+lane;
        for (uint32_t bank = 0; bank < 4; ++bank) file->write(file->slots()-1, 56+bank, UINT32_MAX, values);
        for (uint32_t bank = 0; bank < 4; ++bank) {
            require(!file->writable(56+bank), "second write accepted in bank");
            require(file->read(file->slots()-1, 56+bank) == values, "row packing or bypass differs");
            require(!file->read(0, bank), "second read accepted in bank");
        }
        rejects([&] { file->write(0, 0, 1, values); });
        require(file->stats().clocks == 0, "port operations clocked state");
        file->tick();
        require(file->stats().reads == 4 && file->stats().writes == 4, "accepted port counters differ");
        require(file->read(file->slots()-1, 59) == values, "last bank lost committed data");
        require(file->read(0, 0) == RegisterVector{}, "physical warp slots alias");
    }
}

// Partial writes preserve untouched lanes, including zero initialization after physical slot reuse.
// 部分写入保留未参与 lane，包括物理槽复用后的零初始化。
void masks_zero_and_reuse() {
    auto file = simt::hw::make_register_file(4);
    RegisterVector first, second;
    first.fill(91); second.fill(42);
    file->write(0, 4, UINT32_MAX, first); file->write(1, 5, UINT32_MAX, first); file->tick();
    file->write(0, 4, 0x80000001, second);
    auto expected = first; expected[0] = expected[31] = 42;
    require(file->read(0, 4) == expected, "masked bypass lost old lanes"); file->tick();
    file->clear(0);
    require(file->read(0, 4) == RegisterVector{}, "clear exposed stale data before edge");
    require(file->read(1, 5) == first, "clear damaged another slot"); file->tick();
    file->write(0, 4, 2, second); expected.fill(0); expected[1] = 42;
    require(file->read(0, 4) == expected, "reused slot retained stale unmasked lanes");
    file->write(0, 63, UINT32_MAX, first);
    for (uint32_t query = 0; query < 16; ++query)
        require(file->read(0, 63) == RegisterVector{} && file->writable(63), "R63 used physical port");
    file->tick();
    require(file->stats().cleared_slots == 1, "slot clear count differs");
}

// Queries, rejected calls, and independent files cannot mutate clocks or another file's data.
// 查询、被拒绝调用和独立实例不能修改时钟或另一实例的数据。
void clocks_errors_and_isolation() {
    auto first = simt::hw::make_register_file(4), second = simt::hw::make_register_file(4);
    RegisterVector values; values.fill(99);
    for (uint32_t idle = 0; idle < 20; ++idle) first->tick();
    require(first->stats().active_clocks == 0, "idle storage clock gate opened");
    first->write(0, 2, 7, values);
    for (uint32_t query = 0; query < 100; ++query) require(!first->writable(2), "query changed port occupancy");
    rejects([&] { first->clear(0); });
    rejects([&] { first->read(8, 0); });
    rejects([&] { first->write(0, 64, 1, values); });
    require(first->stats().clocks == 20 && second->stats().clocks == 0, "query or peer advanced clocks");
    require(second->read(0, 2) == RegisterVector{}, "independent files share data");
    first->tick();
    require(first->stats().active_clocks == 1, "write did not clock storage exactly once");
    rejects([] { simt::hw::make_register_file(3); });
    rejects([] { simt::hw::make_register_file(4, 3); });
    auto shared = simt::hw::make_register_file(4, 1);
    shared->write(0, 0, 1, values);
    require(!shared->writable(1), "shared write port group admitted a second bank");
    require(shared->port_groups() == 1, "port sharing configuration lost");
    shared->read(0, 1);
    rejects([&] { shared->write(0, 2, 1, values); });
    require(!shared->read(0, 2), "shared read port group admitted a second bank");
}

// A pair of predicate muxes shares one issue capture; masked writes and slot reuse remain physical.
// 两个谓词多路器共用一次发射捕获；掩码写入与槽复用均保留物理存储语义。
void predicate_storage_and_ports() {
    for (uint32_t partitions : {1,2,4,8}) {
        auto file = simt::hw::make_register_file(partitions); const auto slot = file->slots()-1;
        rejects([&] { file->accept_predicates(false); });
        file->write_predicate(slot,6,UINT32_MAX,0x80000001);
        rejects([&] { file->write_predicate(slot,5,1,1); });
        const auto value = file->probe_predicates(slot,6,UINT32_MAX,6);
        require(value.participating == 0x80000001 && value.source == 0x80000001,"predicate bypass differs");
        file->accept_predicates(true);
        rejects([&] { file->probe_predicates(0,7,UINT32_MAX); });
        rejects([&] { file->clear(slot); }); file->tick();
        require(file->probe_predicates(slot,14,UINT32_MAX,6).participating == 0x7ffffffe,"predicate commit/invert differs");
        require(file->probe_predicates(0,6,UINT32_MAX,7).source == UINT32_MAX,"P7 is not constant");
        file->clear(slot);
        require(file->probe_predicates(slot,6,UINT32_MAX,6).source == 0,"reused slot leaked predicates"); file->tick();
        file->write_predicate(slot,6,2,2); file->tick();
        require(file->probe_predicates(slot,6,UINT32_MAX,6).source == 2,"partial write resurrected stale lanes");
        rejects([&] { file->write_predicate(slot,7,1,1); });
        rejects([&] { file->probe_predicates(file->slots(),7,1); });
        rejects([&] { file->probe_predicates(0,16,1); });
        rejects([&] { file->probe_predicates(0,7,1,8); });
        require(file->stats().clocks == 3 && file->stats().guard_reads == 1 && file->stats().predicate_source_reads == 1 &&
                file->stats().predicate_writes == 2,"predicate events or hidden clocks differ");
    }
}

void run_cases() {
    predicate_storage_and_ports(); std::cout << "PASS predicate storage ports masks and isolation across four geometries\n";
    geometries_and_port_capacity(); std::cout << "PASS register geometries and physical ports\n";
    masks_zero_and_reuse(); std::cout << "PASS register masks zero and slot reuse\n";
    clocks_errors_and_isolation(); std::cout << "PASS register clocks errors and isolation\n";
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

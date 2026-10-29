#include "simt/writeback.hpp"
#include "simt/operand_collector.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace simt::hw;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed,"invalid writeback operation accepted");
}
RegisterVector values(uint32_t start) {
    RegisterVector result;
    for (uint32_t lane = 0; lane < 32; ++lane) result[lane] = start+lane;
    return result;
}

// Complete through the physical output port, then consume the same-cycle RF bypass through a collector.
// 经物理输出端口完成，再通过收集器消费同拍寄存器堆旁路。
void direct_register_route_and_lane_masks() {
    for (uint32_t partitions : {1,2,4,8}) {
        auto file = make_register_file(partitions,4,4); auto& wb = *file->writeback(); auto& col = *file->collector();
        const auto slot = file->slots()-1;
        file->write(slot,59,UINT32_MAX,values(100)); file->tick();
        const auto id = wb.reserve({slot,6,1,59,0x80000001});
        const RegisterUse sources[] = {{59,UINT32_MAX}};
        const auto reader = col.allocate(slot,sources); file->tick();
        const auto clocks = file->stats().clocks;
        require(wb.complete(id,6,values(900)),"completion refused"); wb.arbitrate();
        require(wb.take(id) == values(900),"physical retirement data differs");
        require(!wb.selected(id),"double retirement allowed");
        col.collect(); col.dispatch(UINT32_MAX);
        auto expected = values(100); expected[0] = 900; expected[31] = 931;
        require(col.take(reader)[0].values == expected,"RTL writeback did not reach RF bypass or lane masks");
        require(file->stats().clocks == clocks,"completion inserted hidden clocks"); file->tick();
        const auto saved = col.allocate(slot,sources); file->tick(); col.collect(); col.dispatch(UINT32_MAX);
        require(col.take(saved)[0].values == expected,"RTL writeback did not persist in RF"); file->tick();
        require(file->stats().writes == 2,"physical RF writes not counted");
        require(wb.occupancy() == 0 && wb.writeback_stats().retired == 1,"writeback entry leaked");
    }
}

void held_results_and_phase_errors() {
    auto file = make_register_file(4,1,4); auto& wb = *file->writeback();
    rejects([&] { wb.reserve({0,0,2,7,1}); });
    const auto first = wb.reserve({0,0,1,1,UINT32_MAX}); file->tick();
    const auto second = wb.reserve({1,1,1,2,UINT32_MAX}); file->tick();
    rejects([&] { wb.complete(second,0,values(0)); });
    require(wb.complete(first,0,values(100)),"old completion rejected");
    require(!wb.complete(first,0,values(999)),"producer port used twice");
    require(wb.complete(second,1,values(200)),"young completion rejected");
    rejects([&] { wb.take(first); }); wb.arbitrate();
    rejects([&] { wb.complete(second,1,values(999)); });
    rejects([&] { wb.take(second); });
    require(wb.take(first) == values(100),"oldest result lost"); file->tick();
    const auto active = wb.writeback_stats().active_clocks;
    for (uint32_t idle = 0; idle < 15; ++idle) { wb.arbitrate(); require(wb.selected(second),"held result lost"); file->tick(); }
    require(wb.writeback_stats().active_clocks == active,"held result clock gate did not close");
    rejects([&] { file->write(0,3,1,values(0)); });
    file->collector()->collect(); rejects([&] { wb.take(second); }); file->tick();
    wb.arbitrate(); require(wb.take(second) == values(200),"held payload was overwritten"); file->tick();
    require(wb.writeback_stats().completed == 2 && wb.writeback_stats().retired == 2,"completion counts differ");
}

void full_table_and_reuse() {
    auto file = make_register_file(4,4,4); auto& wb = *file->writeback();
    std::array<uint32_t,32> ids;
    for (uint32_t i = 0; i < ids.size(); ++i) { ids[i] = wb.reserve({0,i%7,0,0,0}); file->tick(); }
    require(!wb.available() && wb.occupancy() == 32,"finite completion capacity absent");
    rejects([&] { wb.reserve({0,0,0,0,0}); });
    wb.complete(ids[0],0,{}); wb.arbitrate(); wb.take(ids[0]);
    require(wb.available() && wb.reserve({0,6,0,0,0}) == ids[0],"acknowledgement failed to return credit"); file->tick();
    require(wb.occupancy() == 32,"same-cycle release/reserve lost capacity");
}

void run_cases() {
    direct_register_route_and_lane_masks(); std::cout << "PASS writeback actual RF bypass, storage and lane masks across four geometries\n";
    held_results_and_phase_errors(); std::cout << "PASS writeback retained results, finite ports and phase errors\n";
    full_table_and_reuse(); std::cout << "PASS writeback 32-slot capacity and same-cycle reuse\n";
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

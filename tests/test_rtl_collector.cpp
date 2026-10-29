#include "simt/operand_collector.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace simt::hw;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, "invalid collector operation accepted");
}

void masks_retention_and_arbitration() {
    auto file = make_register_file(4, 4, 4); auto& collector = *file->collector();
    RegisterVector old, newer; old.fill(11); newer.fill(99);
    file->write(0, 0, UINT32_MAX, old); file->tick();
    file->write(0, 4, UINT32_MAX, newer); file->tick();
    const RegisterUse sources[] = {{0,1},{4,0x80000000}};
    const auto id = collector.allocate(0, sources); file->tick();
    auto grants = collector.collect();
    require(grants.size() == 1 && grants[0].reg == 0 && !collector.ready(id), "same-bank sources did not serialize");
    file->tick();
    file->write(0, 0, UINT32_MAX, newer);
    grants = collector.collect();
    require(grants.size() == 1 && grants[0].reg == 4 && collector.ready(id), "second source did not complete");
    file->tick();
    const auto active = collector.collector_stats().active_clocks;
    for (uint32_t idle = 0; idle < 20; ++idle) {
        require(collector.collect().empty() && collector.ready(id), "ready held operand generated another read");
        file->tick();
    }
    require(collector.collector_stats().active_clocks == active, "held operand clock gate did not remain idle");
    collector.collect();
    const auto values = collector.take(id);
    require(values.size() == 2 && values[0].values[0] == 11 && values[0].values[1] == 0,
            "retained value followed younger RF overwrite or lane mask leaked");
    require(values[1].values[31] == 99 && values[1].values[0] == 0, "second operand mask lost");
    file->tick(); require(collector.occupancy() == 0, "collector slot leaked");
}

void full_capacity_same_cycle_reuse_and_errors() {
    auto file = make_register_file(4, 1, 2); auto& collector = *file->collector();
    const RegisterUse source[] = {{3,7}};
    const auto older = collector.allocate(0, source); file->tick();
    const auto younger = collector.allocate(1, source); file->tick();
    require(!collector.available() && collector.occupancy() == 2, "collector capacity not enforced");
    auto grants = collector.collect();
    require(grants.size() == 1 && grants[0].entry == older, "younger entry bypassed oldest source");
    rejects([&] { collector.take(younger); });
    collector.take(older);
    require(collector.available(), "same-cycle release failed to return capacity");
    require(collector.allocate(2, {}) == older, "physical entry was not reused");
    rejects([&] { collector.allocate(3, {}); });
    file->tick(); collector.collect();
    require(collector.ready(older) && collector.ready(younger), "zero-source or surviving entry lost");
    collector.take(older); collector.take(younger); file->tick();
    const RegisterUse duplicate[] = {{2,1},{2,2}};
    rejects([&] { collector.allocate(0, duplicate); });
    require(collector.available(), "rejected descriptor poisoned allocation port");
    rejects([&] { file->read(0, 0); });
}

void geometries_clocks_and_independent_files() {
    for (uint32_t parts : {1,2,4,8}) {
        auto file = make_register_file(parts, 4, 32); auto& collector = *file->collector();
        for (uint32_t idle = 0; idle < 20; ++idle) { collector.collect(); file->tick(); }
        require(collector.collector_stats().active_clocks == 0, "empty collector clock gate opened");
        RegisterVector values; for (uint32_t lane = 0; lane < 32; ++lane) values[lane] = 0xabcd0000+lane;
        file->write(file->slots()-1, 59, UINT32_MAX, values); file->tick();
        const RegisterUse sources[] = {{59,UINT32_MAX}};
        const auto id = collector.allocate(file->slots()-1, sources); file->tick();
        const auto clocks = file->stats().clocks;
        for (uint32_t query = 0; query < 100; ++query) require(!collector.ready(id), "query collected source without service");
        collector.collect();
        require(collector.take(id)[0].values == values, "last slot/bank packed address or data lost");
        require(file->stats().clocks == clocks, "query/collect/take inserted a clock");
        file->tick();
        require(collector.collector_stats().allocated == 1 && collector.collector_stats().released == 1,
                "handshake counters differ");
        auto peer = make_register_file(parts,4,4);
        require(peer->collector()->occupancy() == 0 && peer->stats().clocks == 0, "independent SM state leaked");
    }
}

void run_cases() {
    masks_retention_and_arbitration(); std::cout << "PASS collector masked old-value retention and bank arbitration\n";
    full_capacity_same_cycle_reuse_and_errors(); std::cout << "PASS collector capacity same-cycle reuse and rejected commands\n";
    geometries_clocks_and_independent_files(); std::cout << "PASS collector physical geometries clocks and isolation\n";
}
int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

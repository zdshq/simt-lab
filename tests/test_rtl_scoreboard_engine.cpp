#include "simt/scoreboard_engine.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using simt::hw::ScoreboardEngine;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Action> void rejects(Action action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, "invalid or overflowing operation accepted");
}

void clocks_and_rmw() {
    auto board = simt::hw::make_scoreboard_engine(true);
    require(board->name() == "rtl", "backend identity");
    for (uint32_t idle = 0; idle < 100; ++idle) board->tick();
    require(board->cycle() == 100 && board->active_cycles() == 0, "empty event bus did not gate state clocks");
    board->reserve_read(4, 7); board->reserve_write(4, 7);
    for (uint32_t query = 0; query < 100; ++query) {
        require(!board->readable(4, 7) && board->readable(4, 8), "lane writer ownership");
        require(board->cycle() == 100 && board->active_cycles() == 0, "query advanced clock");
    }
    board->tick();
    board->retire_write(4, 7); board->collect_read(4, 7);
    require(board->writable(4, 7), "same-cycle completion forwarding");
    board->reserve_write(4, 7); board->tick();
    require(board->cycle() == 102 && board->active_cycles() == 2 && !board->writable(4, 7), "edge lost replacement writer");
}

uint32_t target(uint32_t port) { return (port*5+7)%63; }

void all_event_ports_and_word_boundaries() {
    auto board = simt::hw::make_scoreboard_engine(true);
    for (uint32_t port = 0; port < 16; ++port) board->reserve_write(target(port), 1u << port);
    rejects([&] { board->reserve_write(62, 1); });
    require(board->cycle() == 0, "full event bus inserted hidden clock");
    board->tick();
    for (uint32_t port = 0; port < 16; ++port) {
        require(!board->readable(target(port), 1u << port), "packed event target or mask corrupted");
        board->retire_write(target(port), 1u << port);
    }
    board->tick();
    for (uint32_t reg = 0; reg < 64; ++reg) require(board->writable(reg, UINT32_MAX), "event port leaked ownership");
}

void errors_preserve_pending_events() {
    auto board = simt::hw::make_scoreboard_engine(true);
    board->reserve_read(9, 1); board->tick();
    rejects([&] { board->collect_read(9, 3); });
    require(!board->writable(9, 1), "partial invalid mask released reader");
    board->collect_read(9, 1); board->reserve_predicate(2, 7);
    rejects([&] { board->retire_predicate(2, 15); });
    require(!board->predicate_ready(2, 7), "invalid predicate retirement changed state");
    board->retire_predicate(2, 7); board->tick();
    require(board->writable(9, 3) && board->predicate_ready(2, 7), "rejected event poisoned later ports");
    rejects([&] { board->readable(64, 1); });
    rejects([&] { board->reserve_write(64, 1); });
}

void independent_slots_and_finite_readers() {
    auto first = simt::hw::make_scoreboard_engine(true), second = simt::hw::make_scoreboard_engine(true);
    for (uint32_t reader = 0; reader < 63; ++reader) {
        first->reserve_read(3, 1);
        first->tick();
    }
    first->tick();
    rejects([&] { first->reserve_read(3, 1); });
    require(second->writable(3, 1) && second->cycle() == 0, "physical slots share state or clocks");
    first->collect_read(3, 1); first->reserve_read(3, 1); first->tick();
    for (uint32_t reader = 0; reader < 63; ++reader) {
        first->collect_read(3, 1);
        first->tick();
    }
    first->tick();
    require(first->writable(3, 1), "reader saturation wrapped state");
}

void dependency_ports_and_observation() {
    for (bool rtl:{false,true}) {
        auto board=simt::hw::make_scoreboard_engine(rtl);
        board->reserve_write(2,1); board->reserve_write(62,0x80000000); board->reserve_read(4,4);
        board->reserve_predicate(0,8); board->reserve_predicate(6,16); board->tick();
        simt::hw::DependencyQuery q;
        q.registers={2,63,62}; q.masks={1,UINT32_MAX,0x80000000}; q.destination=4; q.write_mask=4;
        q.predicate=0; q.predicate_mask=8; q.source_predicate=6; q.source_predicate_mask=16;
        const auto status=board->dependencies(q);
        require(!status.ready && status.blocked==117,"parallel hazard classes or source order corrupted");
        require(!board->issue_ready(q),"busy instruction accepted");
        const auto& stats=board->dependency_stats();
        require(stats.queries==1 && stats.blocked==1 && stats.raw==1 && stats.war==1 && stats.predicate==1 && !stats.waw,
            "query observation differs from circuit categories");
        for (uint32_t reg=0; reg<64; ++reg) {
            static_cast<void>(board->readable(reg,UINT32_MAX));
            require(board->dependencies(q).blocked==117 && board->cycle()==1,"independent query changed pins or clock");
        }
        q.masks={2,UINT32_MAX,1}; q.write_mask=8; q.predicate_mask=16; q.source_predicate_mask=8;
        require(board->issue_ready(q),"disjoint lanes blocked");
    }
}

void dependency_forwarding_and_rejection() {
    auto board=simt::hw::make_scoreboard_engine(true); simt::hw::DependencyQuery q;
    q.registers={5,63,63}; q.masks={1,0,0}; q.destination=5; q.write_mask=1;
    board->reserve_read(5,1); board->reserve_write(5,1); board->tick();
    require(board->dependencies(q).blocked==25,"RMW classes merged");
    rejects([&]{board->collect_read(5,3);});
    require(board->dependencies(q).blocked==25,"rejected release reached dependency ports");
    board->retire_write(5,1); board->collect_read(5,1);
    require(board->dependencies(q).ready && board->cycle()==1,"same-cycle release absent");
    board->reserve_read(5,1); board->reserve_write(5,1);
    require(board->dependencies(q).blocked==25,"same-cycle replacement absent"); board->tick();
    require(board->dependencies(q).blocked==25 && board->cycle()==2,"replacement changed after edge");
}

void dependency_bounds_and_slot_isolation() {
    auto first=simt::hw::make_scoreboard_engine(true),second=simt::hw::make_scoreboard_engine(true);
    simt::hw::DependencyQuery q; q.registers[2]=62; q.masks[2]=0x80000000;
    first->reserve_write(62,0x80000000);
    require(first->dependencies(q).blocked==4 && second->dependencies(q).ready,"slots share query state");
    for (uint32_t field=0; field<6; ++field) {
        auto bad=q;
        if (field<3) bad.registers[field]=64;
        else if (field==3) bad.destination=64;
        else if (field==4) bad.predicate=8;
        else bad.source_predicate=8;
        rejects([&]{first->dependencies(bad);});
        require(first->dependencies(q).blocked==4,"invalid query corrupted valid pending reservations");
    }
    require(first->cycle()==0 && second->cycle()==0,"query or rejected pins introduced a clock");
}

void run_cases() {
    clocks_and_rmw(); std::cout << "PASS scoreboard clocks and read-modify-write\n";
    all_event_ports_and_word_boundaries(); std::cout << "PASS scoreboard event pins and finite capacity\n";
    errors_preserve_pending_events(); std::cout << "PASS scoreboard rejected event isolation\n";
    independent_slots_and_finite_readers(); std::cout << "PASS scoreboard slot isolation and reader credits\n";
    dependency_ports_and_observation(); std::cout << "PASS scoreboard parallel dependency masks and observation\n";
    dependency_forwarding_and_rejection(); std::cout << "PASS scoreboard dependency forwarding and rejection\n";
    dependency_bounds_and_slot_isolation(); std::cout << "PASS scoreboard dependency bounds and independent slots\n";
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

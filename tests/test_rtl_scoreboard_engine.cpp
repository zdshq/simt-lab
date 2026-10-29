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

void run_cases() {
    clocks_and_rmw(); std::cout << "PASS scoreboard clocks and read-modify-write\n";
    all_event_ports_and_word_boundaries(); std::cout << "PASS scoreboard event pins and finite capacity\n";
    errors_preserve_pending_events(); std::cout << "PASS scoreboard rejected event isolation\n";
    independent_slots_and_finite_readers(); std::cout << "PASS scoreboard slot isolation and reader credits\n";
}

int run_application() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main() { return run_application(); }

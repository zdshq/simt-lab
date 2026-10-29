#include "simt/hardware_pipeline.hpp"
#include <stdexcept>

namespace simt::hw {
bool Scoreboard::readable(uint32_t reg, uint32_t mask) const {
    return reg == 63 || !(writes_.at(reg) & mask);
}

bool Scoreboard::writable(uint32_t reg, uint32_t mask) const {
    if (reg == 63) return true;
    if (!readable(reg, mask)) return false;
    for (uint32_t lane = 0; lane < 32; ++lane)
        if ((mask & (uint32_t{1} << lane)) && readers_.at(reg)[lane]) return false;
    return true;
}

bool Scoreboard::predicate_ready(uint32_t pred, uint32_t mask) const {
    return pred == 7 || !(predicates_.at(pred) & mask);
}

void Scoreboard::reserve_read(uint32_t reg, uint32_t mask) {
    if (reg == 63) return;
    if (!readable(reg, mask)) throw std::runtime_error("scoreboard_read_before_writeback");
    for (uint32_t lane = 0; lane < 32; ++lane)
        if (mask & (uint32_t{1} << lane)) ++readers_.at(reg)[lane];
}

void Scoreboard::collect_read(uint32_t reg, uint32_t mask) {
    if (reg == 63) return;
    for (uint32_t lane = 0; lane < 32; ++lane) {
        if (!(mask & (uint32_t{1} << lane))) continue;
        auto& count = readers_.at(reg)[lane];
        if (!count) throw std::runtime_error("scoreboard_unowned_read");
        --count;
    }
}

// A read-modify-write may own both a source read and a destination write of the same register.
// 读改写指令可同时持有同一寄存器的源读取与目标写入。
void Scoreboard::reserve_write(uint32_t reg, uint32_t mask) {
    if (reg == 63) return;
    if (!readable(reg, mask)) throw std::runtime_error("scoreboard_duplicate_writer");
    writes_.at(reg) |= mask;
}

void Scoreboard::retire_write(uint32_t reg, uint32_t mask) {
    if (reg == 63) return;
    if ((writes_.at(reg) & mask) != mask) throw std::runtime_error("scoreboard_unowned_write");
    writes_.at(reg) &= ~mask;
}

void Scoreboard::reserve_predicate(uint32_t pred, uint32_t mask) {
    if (pred >= 7 || !predicate_ready(pred, mask)) throw std::runtime_error("scoreboard_predicate_conflict");
    predicates_.at(pred) |= mask;
}

void Scoreboard::retire_predicate(uint32_t pred, uint32_t mask) {
    if ((predicates_.at(pred) & mask) != mask) throw std::runtime_error("scoreboard_unowned_predicate");
    predicates_.at(pred) &= ~mask;
}
}

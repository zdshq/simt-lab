#include "Vdependency_query.h"
#include "bench.hpp"
#include <random>

namespace {
void clear(Vdependency_query& d) {
    for (auto& word:d.writers) word=0;
    for (auto& word:d.readers) word=0;
    for (auto& word:d.predicates) word=0;
    d.source_registers=1|(2<<6)|(62<<12);
    d.source_masks[0]=1; d.source_masks[1]=2; d.source_masks[2]=0x80000000;
    d.destination=9; d.write_mask=4; d.predicate=0; d.predicate_mask=8;
    d.source_predicate=6; d.source_predicate_mask=16;
}

void every_hazard_combination() {
    Vdependency_query d;
    for (uint32_t pattern=0; pattern<128; ++pattern) {
        clear(d);
        if (pattern&1) d.writers[1]=1;
        if (pattern&2) d.writers[2]=2;
        if (pattern&4) d.writers[62]=0x80000000;
        if (pattern&8) d.writers[9]=4;
        if (pattern&16) d.readers[9]=4;
        if (pattern&32) d.predicates[0]=8;
        if (pattern&64) d.predicates[6]=16;
        d.eval();
        require(d.blocked==pattern && bool(d.ready)==(pattern==0),"simultaneous hazard classification differs");
    }
}

void constants_and_mask_isolation() {
    Vdependency_query d; clear(d);
    for (auto& word:d.writers) word=UINT32_MAX;
    for (auto& word:d.readers) word=UINT32_MAX;
    for (auto& word:d.predicates) word=UINT32_MAX;
    d.source_registers=0x3ffff; d.destination=63; d.predicate=7; d.source_predicate=7; d.eval();
    require(d.ready && !d.blocked,"constant zero/true consumed reservation state");
    clear(d); d.writers[1]=~1u; d.writers[2]=~2u; d.writers[62]=0x7fffffff;
    d.readers[9]=~4u; d.writers[9]=~4u; d.predicates[0]=~8u; d.predicates[6]=~16u; d.eval();
    require(d.ready,"disjoint lanes blocked instruction");
    d.source_masks[0]=0; d.source_masks[1]=0; d.source_masks[2]=0;
    d.write_mask=0; d.predicate_mask=0; d.source_predicate_mask=0;
    for (auto& word:d.writers) word=UINT32_MAX;
    for (auto& word:d.readers) word=UINT32_MAX;
    for (auto& word:d.predicates) word=UINT32_MAX;
    d.eval(); require(d.ready,"empty request consumed a lane");
}

// A scalar lane scan is independent of the packed selectors and reductions in the circuit.
// 标量 lane 扫描独立于电路中的打包选择器与归约。
uint32_t reference(const Vdependency_query& d) {
    uint32_t blocked=0;
    for (uint32_t lane=0; lane<32; ++lane) {
        const auto bit=uint32_t{1}<<lane;
        for (uint32_t source=0; source<3; ++source) {
            const auto reg=(d.source_registers>>(source*6))&63;
            if (reg!=63 && (d.source_masks[source]&bit) && (d.writers[reg]&bit)) blocked|=1u<<source;
        }
        if (d.destination!=63 && (d.write_mask&bit)) {
            if (d.writers[d.destination]&bit) blocked|=8;
            if (d.readers[d.destination]&bit) blocked|=16;
        }
        if (d.predicate!=7 && (d.predicate_mask&bit) && (d.predicates[d.predicate]&bit)) blocked|=32;
        if (d.source_predicate!=7 && (d.source_predicate_mask&bit) && (d.predicates[d.source_predicate]&bit)) blocked|=64;
    }
    return blocked;
}

void randomized_reservation_maps() {
    Vdependency_query d; std::mt19937 rng(624813);
    for (uint32_t sample=0; sample<16000; ++sample) {
        clear(d);
        for (auto& word:d.writers) word=uint32_t{1}<<(rng()%32);
        for (auto& word:d.readers) word=uint32_t{1}<<(rng()%32);
        for (auto& word:d.predicates) word=uint32_t{1}<<(rng()%32);
        d.source_registers=rng()&0x3ffff;
        for (auto& mask:d.source_masks) mask=rng();
        d.destination=rng()%64; d.write_mask=rng(); d.predicate=rng()%8; d.predicate_mask=rng();
        d.source_predicate=rng()%8; d.source_predicate_mask=rng(); d.eval();
        const auto expected=reference(d);
        require(d.blocked==expected && bool(d.ready)==(expected==0),"random dependency mask differs");
    }
}

void run_cases() {
    run_case("dependency ports all 128 simultaneous hazard combinations",every_hazard_combination);
    run_case("dependency constants, disjoint lanes and empty masks",constants_and_mask_isolation);
    run_case("dependency 16000 random full reservation maps",randomized_reservation_maps);
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

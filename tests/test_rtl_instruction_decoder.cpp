#include "simt/instruction_decoder.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt::hw;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,std::string_view expected) {
    bool failed=false; try { action(); } catch (const std::exception& e) { failed=e.what()==expected; }
    require(failed,"wrong or missing decoder exception");
}
void raw_fields_and_snapshot_isolation() {
    auto unit=make_instruction_decoder();
    const std::array<uint32_t,4> words{0x02010007,0x123,0,0};
    // IMAD uses all three ordinary sources; guard selection remains an independent predicate stage.
    // IMAD 使用三个普通源；guard 筛选仍属于独立谓词阶段。
    const auto instruction=unit->inspect(words); auto value=instruction; value.imm=123;
    require(instruction.op==simt::Op::IMAD && instruction.ra==1 && instruction.rb==2 && instruction.rc==35 &&
        instruction.guard==1 && instruction.imm!=value.imm,"raw instruction fields or snapshot isolation wrong");
    const auto f=unit->operands(0x80000001);
    require(f.reads.size()==3 && f.reads[2].reg==35 && f.reads[2].mask==0x80000001,"third source wires wrong");
    unit->accept(); require(!unit->stats().cycles && unit->stats().accepted==1,"decode added a hidden clock"); unit->tick();
    require(unit->stats().cycles==1 && unit->stats().sources==3,"interface event counts wrong");
}
void deferred_validation_and_errors() {
    auto unit=make_instruction_decoder();
    unit->inspect({0x00ff0002,0x700,0,0}); require(unit->operands(0).reads.empty(),"inactive ordinary source rejected"); unit->tick();
    unit->inspect({0x00ff0002,0x700,0,0});
    rejects([&]{unit->operands(1);},"register_out_of_range");
    rejects([&]{unit->accept();},"decoder_accept_phase"); unit->tick();
    unit->inspect({0x0000070e,0x700,0,0}); rejects([&]{unit->operands(0);},"predicate_out_of_range"); unit->tick();
    rejects([&]{unit->inspect({255,0,0,0});},"unknown_opcode"); unit->tick();
    rejects([&]{unit->inspect({0,0x700,0,1});},"reserved_encoding_bits"); unit->tick();
    require(unit->stats().encoding_errors==2 && unit->stats().operand_errors==2,"error counts missing");
}
void phases_and_independent_instances() {
    auto a=make_instruction_decoder(),b=make_instruction_decoder();
    rejects([&]{a->operands(1);},"decoder_operand_phase");
    a->inspect({0x00000901,0xa00,0x12345678,0x091a2800});
    rejects([&]{a->inspect({24,0x700,0,0});},"decoder_already_inspected");
    rejects([&]{a->accept();},"decoder_accept_phase");
    b->inspect({24,0xf00,0,0}); const auto bf=b->operands(0);
    const auto af=a->operands(1);
    require(af.destination==9 && !af.boundary && !bf.destination && bf.boundary,"instances contaminated fields");
    rejects([&]{a->operands(2);},"decoder_operand_phase");
    a->accept(); rejects([&]{a->accept();},"decoder_accept_phase");
    require(a->stats().accepted==1 && !b->stats().accepted,"instances shared counters");
}
void source_merging_and_all_guards() {
    auto unit=make_instruction_decoder();
    for (uint32_t guard=0; guard<16; ++guard) {
        auto i=unit->inspect({0x04040220,guard<<8,0x40000000,0});
        require(i.guard==guard,"guard field truncated"); const auto f=unit->operands(1);
        require(f.reads.size()==1 && f.reads[0].reg==4 && f.reads[0].mask==0x40000001 && !f.write_mask,"merged SHFL mask wrong");
        unit->accept(); unit->tick();
    }
    require(unit->stats().inspected==16 && unit->stats().accepted==16,"guard samples not counted");
}
void run_cases() {
    raw_fields_and_snapshot_isolation(); std::cout<<"PASS decoder adapter raw fields, snapshots and interface events\n";
    deferred_validation_and_errors(); std::cout<<"PASS decoder adapter deferred validation and encoding errors\n";
    phases_and_independent_instances(); std::cout<<"PASS decoder adapter finite phases and independent instances\n";
    source_merging_and_all_guards(); std::cout<<"PASS decoder adapter merged cross-lane sources and all guards\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

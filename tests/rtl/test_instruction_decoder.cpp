#include "Vinstruction_decoder.h"
#include "bench.hpp"
#include "simt/instruction_decoder.hpp"
#include <random>

namespace {
using Words=std::array<uint32_t,4>;
void drive(Vinstruction_decoder& d,const Words& words,uint32_t mask=0xffffffff) {
    for (uint32_t at=0; at<4; ++at) d.words[at]=words[at];
    d.participating=mask; d.eval();
}
Words operands(uint32_t op,uint32_t rd=0,uint32_t ra=0,uint32_t rb=0,uint32_t rc=0,uint32_t imm=0) {
    Words words{op|(rd<<8)|(ra<<16)|(rb<<24),rc|0x700,imm,0};
    const auto& info=simt::op_info(static_cast<simt::Op>(op));
    for (uint32_t at=0; at<4; ++at) words[at]&=info.allowed[at];
    return words;
}
void fields(const Vinstruction_decoder& d,const simt::Instruction& i) {
    require(d.opcode==static_cast<uint32_t>(i.op) && d.rd==i.rd && d.ra==i.ra && d.rb==i.rb && d.rc==i.rc &&
        d.guard==i.guard && d.aux==i.aux && d.mode==i.mode && d.imm==i.imm && d.control==i.control,"decoded instruction fields differ");
}
void footprint(const Vinstruction_decoder& d,const simt::Footprint& f) {
    require(d.pipe==static_cast<uint32_t>(f.pipe) && bool(d.boundary)==f.boundary && bool(d.store)==f.store &&
        d.write_mask==f.write_mask && d.source_count==f.reads.size(),"instruction roles/masks differ");
    require(bool(d.destination_valid)==bool(f.destination) && (!f.destination || d.destination==*f.destination),"destination differs");
    require(bool(d.predicate_valid)==bool(f.predicate) && (!f.predicate || d.predicate==*f.predicate),"predicate destination differs");
    require(bool(d.source_predicate_valid)==bool(f.source_predicate) && (!f.source_predicate || d.source_predicate==*f.source_predicate),"predicate source differs");
    for (uint32_t at=0; at<f.reads.size(); ++at)
        require(((d.source_registers>>(at*6))&63)==f.reads[at].reg && d.source_masks[at]==f.reads[at].mask,"compacted operand request differs");
}

void fixed_binary_layout() {
    Vinstruction_decoder d;
    drive(d,{0x7e12ff2c,0x0ab56de1,0xdeadbeef,0xfffff800});
    require(!d.encoding_error && d.opcode==44 && d.rd==255 && d.ra==0x12 && d.rb==0x7e && d.rc==0xe1 &&
        d.guard==13 && d.aux==0x56 && d.mode==0xab && d.imm==0xdeadbeef && d.control==0x1fffff,"fixed 128-bit layout differs");
    drive(d,{0x00000901,0x00000a00,0x12345678,0x091a2800});
    require(!d.encoding_error && !d.operand_error && d.rd==9 && d.guard==10 && d.control==0x12345 &&
        d.destination_valid && d.destination==9 && !d.source_count,"fixed MOVI layout differs");
}

void every_opcode_and_reserved_bit() {
    Vinstruction_decoder d;
    for (const auto& op : simt::op_table) {
        Words words{static_cast<uint32_t>(op.op),0x700,0,0}; drive(d,words); require(!d.encoding_error,"known opcode rejected");
        for (uint32_t bit=8; bit<128; ++bit) {
            auto altered=words; altered[bit/32]|=uint32_t{1}<<(bit%32); drive(d,altered);
            const auto legal=op.allowed[bit/32] & (uint32_t{1}<<(bit%32));
            require(d.encoding_error==(legal ? 0 : 2),"instruction-specific reserved bit accepted/rejected incorrectly");
        }
    }
    for (uint32_t op=59; op<256; ++op) { drive(d,{op,0xffffffff,0xffffffff,0xffffffff}); require(d.encoding_error==1,"unknown opcode priority differs"); }
}

void sources_merge_zero_and_cross_lane_masks() {
    Vinstruction_decoder d;
    drive(d,operands(7,62,5,5,5),0x80000001);
    require(!d.operand_error && d.source_count==1 && (d.source_registers&63)==5 && d.source_masks[0]==0x80000001,"three equal sources not merged");
    drive(d,operands(7,62,63,62,1));
    require(d.source_count==2 && (d.source_registers&63)==62 && ((d.source_registers>>6)&63)==1,"zero source not removed in order");
    drive(d,operands(32,2,4,4,0,0x40000000),1);
    require(d.source_count==1 && d.source_masks[0]==0x40000001 && !d.write_mask,"SHFL cross-lane mask was narrowed");
    drive(d,operands(32,2,4,8,0,0x40000000),0);
    require(d.source_count==1 && d.source_masks[0]==0x40000000 && !d.write_mask,"empty execution lost SHFL member read");
    drive(d,operands(31,62,7,0,0,0xa5),0xff);
    require(d.source_predicate_valid && d.source_predicate==7 && d.source_count==0 && d.write_mask==0xa5,"VOTE predicate treated as ordinary source");
}

void register_bounds_and_deferred_execution_checks() {
    Vinstruction_decoder d;
    drive(d,operands(7,0,255,1,2)); require(d.operand_error==2,"invalid source truncated");
    drive(d,operands(7,0,255,255,255),0); require(!d.operand_error && !d.source_count,"inactive ordinary source incorrectly validated");
    drive(d,operands(1,64),0); require(d.operand_error==2,"inactive destination escaped structural check");
    drive(d,operands(14,7,0,0)); require(d.operand_error==3,"constant predicate accepted as destination");
    drive(d,operands(31,0,8),0); require(d.operand_error==3,"invalid VOTE predicate truncated");
    drive(d,operands(26,255,0,0,0,0xffffffff)); require(!d.operand_error,"token interpreted as register destination");
    drive(d,operands(46,255,0,0,0,0xffffffff)); require(!d.operand_error,"CTA barrier operands prematurely range checked");
}

// The existing C++ footprint remains an independent behavioral reference, never part of the RTL execution path.
// 既有 C++ 操作数模型作为独立行为参考，不参与 RTL 执行路径。
void randomized_fields_and_footprints() {
    Vinstruction_decoder d; std::mt19937 rng(901731);
    for (const auto& info : simt::op_table) for (uint32_t sample=0; sample<1200; ++sample) {
        auto words=operands(static_cast<uint32_t>(info.op),rng()%70,rng()%70,rng()%70,rng()%70,rng());
        words[1]=(words[1]&255) | (rng() & info.allowed[1] & 0x0fffff00); words[3]=rng() & info.allowed[3];
        const uint32_t mask=sample%11==0 ? 0 : rng(); drive(d,words,mask);
        require(!d.encoding_error,"legal random encoding rejected"); const auto i=simt::decode(words); fields(d,i);
        std::optional<simt::Footprint> expected; uint32_t error=0;
        try { expected=simt::instruction_footprint(i,mask); }
        catch (const std::runtime_error& e) {
            const std::string message=e.what();
            if (message=="register_out_of_range") error=2;
            else if (message=="predicate_out_of_range") error=3;
            else throw;
        }
        require(d.operand_error==error,"random structural error differs");
        if (expected) footprint(d,*expected);
    }
}
int run_application() {
    try {
        run_case("decoder fixed 128-bit field layout and highest control bits",fixed_binary_layout);
        run_case("decoder all 59 opcodes, every reserved bit and 197 unknown opcodes",every_opcode_and_reserved_bit);
        run_case("decoder merged sources, zero register and cross-lane masks",sources_merge_zero_and_cross_lane_masks);
        run_case("decoder register bounds and deferred execution checks",register_bounds_and_deferred_execution_checks);
        run_case("decoder 70800 randomized field and footprint references",randomized_fields_and_footprints); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

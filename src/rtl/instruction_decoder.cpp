#include "simt/instruction_decoder.hpp"
#include "Vsimt_instruction_decoder.h"
#include <stdexcept>

namespace simt::hw {
namespace {
// Reject schema opcode renumbering until the handwritten operand circuits are updated with it.
// 规范操作码重新编号时拒绝编译，直到手写操作数电路同步更新。
constexpr bool operand_encoding_matches() {
    constexpr std::array operations{Op::NOP,Op::MOVI,Op::MOV,Op::S2R,Op::IADD,Op::ISUB,Op::IMUL,Op::IMAD,
        Op::AND,Op::OR,Op::XOR,Op::SHL,Op::SHR,Op::SAR,Op::ISETP,Op::FSETP,Op::FADD,Op::FMUL,Op::FFMA,
        Op::FEXP2,Op::FRCP,Op::FRSQRT,Op::I2F,Op::F2I,Op::EXIT,Op::BRA,Op::BSSY,Op::BSYNC,Op::BREAK,Op::YIELD,
        Op::ELECT,Op::VOTE,Op::SHFL,Op::ACTIVEMASK,Op::LDG,Op::STG,Op::LDS,Op::STS,Op::LDL,Op::STL,Op::LDC,Op::LD,Op::ST,
        Op::CVTA,Op::ATOM,Op::RED,Op::BAR_SYNC,Op::BAR_ARRIVE,Op::FENCE,Op::TMEM_ALLOC,Op::TMEM_FREE,Op::LDT,Op::STT,Op::MMA,
        Op::MB_INIT,Op::MB_EXPECT,Op::MB_ARRIVE,Op::MB_WAIT,Op::CP_ASYNC};
    for (uint32_t op=0; op<operations.size(); ++op) if (static_cast<uint32_t>(operations[op])!=op) return false;
    return std::size(op_table)==operations.size();
}
static_assert(operand_encoding_matches());

class RtlInstructionDecoder final : public InstructionDecoder {
public:
    ~RtlInstructionDecoder() override { dut_.final(); }
    Instruction inspect(const std::array<uint32_t,4>& words) override;
    Footprint operands(uint32_t participating) override;
    void accept() override;
    void tick() override { ++stats_.cycles; inspected_=false; operands_=false; accepted_=false; valid_=false; }
    const DecoderStats& stats() const override { return stats_; }
private:
    Vsimt_instruction_decoder dut_;
    DecoderStats stats_;
    bool inspected_=false, operands_=false, accepted_=false, valid_=false;
};

// Execution fields are sampled only from circuit outputs, never from the host's predecoded instruction.
// 执行字段只采样电路输出，不读取宿主预解码指令。
Instruction RtlInstructionDecoder::inspect(const std::array<uint32_t,4>& words) {
    if (inspected_) throw std::runtime_error("decoder_already_inspected");
    inspected_=true; ++stats_.inspected;
    for (uint32_t word=0; word<4; ++word) dut_.words[word]=words[word];
    dut_.participating=0; dut_.eval();
    if (dut_.encoding_error) {
        ++stats_.encoding_errors;
        throw std::runtime_error(dut_.encoding_error==1 ? "unknown_opcode" : "reserved_encoding_bits");
    }
    valid_=true; Instruction result;
    result.op=static_cast<Op>(dut_.opcode); result.rd=dut_.rd; result.ra=dut_.ra; result.rb=dut_.rb; result.rc=dut_.rc;
    result.guard=dut_.guard; result.aux=dut_.aux; result.mode=dut_.mode; result.imm=dut_.imm; result.control=dut_.control;
    return result;
}

Footprint RtlInstructionDecoder::operands(uint32_t participating) {
    if (!inspected_ || operands_ || !valid_) throw std::runtime_error("decoder_operand_phase");
    operands_=true; ++stats_.operands; dut_.participating=participating; dut_.eval();
    if (dut_.operand_error) {
        valid_=false; ++stats_.operand_errors;
        throw std::runtime_error(dut_.operand_error==2 ? "register_out_of_range" :
            dut_.operand_error==3 ? "predicate_out_of_range" : "timing_unit_not_connected");
    }
    Footprint result; result.pipe=static_cast<Pipe>(dut_.pipe); result.boundary=dut_.boundary; result.store=dut_.store;
    result.write_mask=dut_.write_mask;
    if (dut_.destination_valid) result.destination=dut_.destination;
    if (dut_.predicate_valid) result.predicate=dut_.predicate;
    if (dut_.source_predicate_valid) result.source_predicate=dut_.source_predicate;
    for (uint32_t source=0; source<dut_.source_count; ++source)
        result.reads.push_back({(dut_.source_registers>>(source*6))&63u,dut_.source_masks[source]});
    stats_.sources+=dut_.source_count; return result;
}
void RtlInstructionDecoder::accept() {
    if (!operands_ || !valid_ || accepted_) throw std::runtime_error("decoder_accept_phase");
    accepted_=true; ++stats_.accepted;
}
}
std::unique_ptr<InstructionDecoder> make_instruction_decoder() { return std::make_unique<RtlInstructionDecoder>(); }
}

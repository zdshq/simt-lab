#pragma once
#include "isa.hpp"
#include "simt/hardware_pipeline.hpp"
#include <memory>

namespace simt {
enum class Pipe { integer, floating, special, memory, control, tensor, async };
struct Footprint {
    Pipe pipe = Pipe::integer;
    std::vector<hw::RegisterUse> reads;
    std::optional<uint32_t> destination, predicate;
    std::optional<uint32_t> source_predicate;
    uint32_t write_mask = 0;
    bool boundary = false, store = false;
};
Footprint instruction_footprint(const Instruction& instruction,uint32_t mask);

namespace hw {
struct DecoderStats {
    uint64_t cycles=0, inspected=0, operands=0, accepted=0, sources=0, encoding_errors=0, operand_errors=0;
};

// One combinational decoder serves each partition; host counters observe interface events, not RTL storage.
// 每分区使用一个组合译码器；宿主计数观测接口事件，不属于 RTL 存储。
class InstructionDecoder {
public:
    virtual ~InstructionDecoder() = default;
    virtual Instruction inspect(const std::array<uint32_t,4>& words) = 0;
    virtual Footprint operands(uint32_t participating) = 0;
    virtual void accept() = 0;
    virtual void tick() = 0;
    virtual const DecoderStats& stats() const = 0;
};
std::unique_ptr<InstructionDecoder> make_instruction_decoder();
}
}

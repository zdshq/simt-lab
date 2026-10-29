#include "simt/timing.hpp"
#include <algorithm>
#include <stdexcept>

namespace simt {
namespace {
void source(Footprint& f, uint32_t reg, uint32_t mask) {
    if (reg == zero_register || !mask) return;
    if (reg >= registers) throw std::runtime_error("register_out_of_range");
    for (auto& use : f.reads)
        if (use.reg == reg) { use.mask |= mask; return; }
    f.reads.push_back({reg, mask});
}

// Scalar source operands are decoded explicitly; immediate and special-register reads use no RF port.
// 显式解码标量源操作数，立即数与特殊寄存器读取不占普通寄存器堆端口。
void scalar(Footprint& f, const Instruction& i, uint32_t mask) {
    if (i.op == Op::NOP) return;
    if (i.op == Op::ISETP || i.op == Op::FSETP) f.predicate = i.rd;
    else f.destination = i.rd;
    if (i.op == Op::MOVI || i.op == Op::S2R) return;
    source(f, i.ra, mask);
    if (i.op != Op::MOV && !(i.op >= Op::FEXP2 && i.op <= Op::F2I)) source(f, i.rb, mask);
    if (i.op == Op::IMAD || i.op == Op::FFMA) source(f, i.rc, mask);
    if (i.op == Op::FSETP || (i.op >= Op::FADD && i.op <= Op::F2I)) f.pipe = Pipe::floating;
    if (i.op >= Op::FEXP2 && i.op <= Op::FRSQRT) f.pipe = Pipe::special;
}

void collective(Footprint& f, const Instruction& i, uint32_t mask) {
    f.pipe = Pipe::control; f.boundary = true;
    f.write_mask = i.op == Op::ACTIVEMASK ? mask : mask & i.imm;
    if (i.op == Op::ELECT) f.predicate = i.rd; else f.destination = i.rd;
    if (i.op == Op::VOTE) f.source_predicate = i.ra;
    if (i.op == Op::SHFL) { source(f, i.ra, mask | i.imm); source(f, i.rb, mask); }
}

void memory(Footprint& f, const Instruction& i, uint32_t mask) {
    f.pipe = Pipe::memory;
    f.store = i.op == Op::STG || i.op == Op::STS || i.op == Op::STL || i.op == Op::ST;
    source(f, i.ra, mask);
    if (f.store) source(f, i.rb, mask); else f.destination = i.rd;
}

// Atomics share warp memory ordering and hold their old-value destination until acknowledgement.
// 原子操作遵循 warp 访存顺序，并持有旧值目标寄存器直到确认完成。
void atomic(Footprint& f, const Instruction& i, uint32_t mask) {
    f.pipe = Pipe::memory; f.store = true;
    source(f, i.ra, mask); source(f, i.rb, mask); source(f, i.rc, mask);
    if (i.op == Op::ATOM) f.destination = i.rd;
}

// Tensor commands serialize their issuing warp while the SM unit completes asynchronously.
// 张量命令串行化发起 warp，SM 单元则异步推进到完成。
void tensor(Footprint& f, const Instruction& i, uint32_t mask) {
    f.pipe = Pipe::tensor; f.boundary = true;
    if (i.op == Op::TMEM_ALLOC || i.op == Op::LDT) f.destination = i.rd;
    if (i.op != Op::TMEM_ALLOC) source(f, i.ra, mask);
    if (i.op == Op::LDT || i.op == Op::STT) source(f, i.rb, mask);
    if (i.op == Op::STT) source(f, i.rc, mask);
}

// DMA submission reads addresses through RF ports but does not reserve the copied payload in registers.
// DMA 提交通过寄存器堆端口读取地址，不在寄存器中预约搬运载荷。
void asynchronous(Footprint& f, const Instruction& i, uint32_t mask) {
    f.pipe = Pipe::async; f.boundary = true;
    source(f, i.ra, mask);
    if (i.op == Op::CP_ASYNC || i.op == Op::MB_WAIT) source(f, i.rb, mask);
    if (i.op == Op::CP_ASYNC) source(f, i.rc, mask);
    if (i.op == Op::MB_ARRIVE) f.destination = i.rd;
}

void validate(const Footprint& f) {
    if (f.destination && *f.destination >= registers) throw std::runtime_error("register_out_of_range");
    if (f.predicate && *f.predicate >= 7) throw std::runtime_error("predicate_out_of_range");
    if (f.source_predicate && *f.source_predicate >= 8) throw std::runtime_error("predicate_out_of_range");
}
}

// Unsupported units fail explicitly until they have a real timing data path.
// 未接入真实时序数据通路的单元明确失败，不回退到瞬时功能计算。
Footprint instruction_footprint(const Instruction& i, uint32_t mask) {
    Footprint f; f.write_mask = mask;
    if (i.op <= Op::F2I) scalar(f, i, mask);
    else if (i.op >= Op::EXIT && i.op <= Op::YIELD) { f.pipe = Pipe::control; f.boundary = true; }
    else if (i.op >= Op::ELECT && i.op <= Op::ACTIVEMASK) collective(f, i, mask);
    else if (i.op >= Op::LDG && i.op <= Op::ST) memory(f, i, mask);
    else if (i.op == Op::CVTA) { source(f, i.ra, mask); f.destination = i.rd; }
    else if (i.op == Op::ATOM || i.op == Op::RED) atomic(f, i, mask);
    else if (i.op >= Op::BAR_SYNC && i.op <= Op::FENCE) { f.pipe = Pipe::control; f.boundary = true; }
    else if (i.op >= Op::TMEM_ALLOC && i.op <= Op::MMA) tensor(f, i, mask);
    else if (i.op >= Op::MB_INIT && i.op <= Op::CP_ASYNC) asynchronous(f, i, mask);
    else throw std::runtime_error(std::string("timing_unit_not_connected: ") + op_name(i.op));
    validate(f);
    return f;
}
}

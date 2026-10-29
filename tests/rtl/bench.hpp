#pragma once
#include <verilated.h>
#if VM_TRACE
#include <verilated_vcd_c.h>
#endif
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

inline void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

// Both clock edges are recorded; tests sample handshakes before the rising edge.
// 记录两个时钟边沿；测试在上升沿之前采样握手。
template<class Model> class Bench {
public:
    Model dut;
    uint64_t cycle = 0;
    explicit Bench(const char* path) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        dut.trace(&trace_, 3);
        trace_.open(path);
#else
        static_cast<void>(path);
#endif
        dut.clk = 0; dut.reset = 1;
        tick(); tick(); dut.reset = 0; dut.eval();
    }
    ~Bench() { dut.final(); }
    void tick() {
        dut.clk = 0; dut.eval(); dump(cycle*2);
        dut.clk = 1; dut.eval(); dump(cycle*2+1);
        ++cycle;
        dut.clk = 0; dut.eval();
    }
private:
#if VM_TRACE
    VerilatedVcdC trace_;
#endif
    // The old trace runtime uses unaligned accesses; sanitizer checks execute the same design without tracing.
    // 旧版波形运行库使用非对齐访问；sanitizer 检查执行同一设计，但不记录波形。
    void dump(uint64_t time) {
#if VM_TRACE
        trace_.dump(time);
#else
        static_cast<void>(time);
#endif
    }
};

template<class Action> void run_case(const char* name, Action action) {
    action();
    std::cout << "PASS " << name << '\n';
}

#include "simt/machine.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace simt;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
Module program() {
    const std::array<std::array<uint32_t,4>,3> words{{{1,0x700,0x23456789,0},{0x003f0023,0x00400700,0,0},{24,0x700,0,0}}};
    std::vector<uint8_t> bytes;
    for (const auto& instruction : words) for (auto word : instruction)
        for (uint32_t byte=0; byte<4; ++byte) bytes.push_back((word>>(byte*8))&255);
    return Module::from_bytes(bytes);
}
std::string execute(Module module,uint32_t expected) {
    Config config; config.sms=1; config.threads=1; config.timing=true;
    config.pipeline.register_rtl=true; config.pipeline.collector_rtl=true;
    auto global=std::make_shared<Memory>(); Machine machine(std::move(module),config,global); machine.run();
    require(global->read(0,4)==expected,"raw instruction did not drive stored result");
    std::ostringstream out; machine.dump(out,true); return out.str();
}

// Poison the host decode while keeping the original binary; real issue must still use raw circuit fields.
// 污染宿主解码但保留原始二进制；实际发射仍必须使用原始电路字段。
void predecoded_copy_cannot_drive_execution() {
    const auto baseline=execute(program(),0x23456789); auto module=program();
    module.code[0].op=Op::FADD; module.code[0].guard=15; module.code[0].rd=62; module.code[0].ra=255;
    module.code[0].imm=0xdeadbeef; module.code[1].rb=20;
    require(execute(std::move(module),0x23456789)==baseline,"host predecoded copy leaked into RTL instruction path");
}
void raw_image_controls_execution() {
    auto module=program(); module.image[0][2]=0x13579bdf;
    static_cast<void>(execute(std::move(module),0x13579bdf));
}
void malformed_raw_images_and_missing_image() {
    for (uint32_t scenario=0; scenario<3; ++scenario) {
        auto module=program(); std::string_view expected;
        if (scenario==0) { module.image[0][0]=255; expected="unknown_opcode"; }
        else if (scenario==1) { module.image[0][3]=1; expected="reserved_encoding_bits"; }
        else { module.image.clear(); expected="instruction_image_missing"; }
        bool rejected=false;
        try { static_cast<void>(execute(std::move(module),0x23456789)); }
        catch (const std::exception& e) { rejected=std::string_view(e.what()).find(expected)!=std::string_view::npos; }
        require(rejected,"invalid raw image bypassed hardware or missing-image validation");
    }
}
void run_cases() {
    predecoded_copy_cannot_drive_execution(); std::cout<<"PASS decoder execution ignores poisoned host instruction snapshots\n";
    raw_image_controls_execution(); std::cout<<"PASS decoder execution consumes actual raw machine-word changes\n";
    malformed_raw_images_and_missing_image(); std::cout<<"PASS decoder execution rejects malformed or missing raw images\n";
}
int run_application() {
    try { run_cases(); return 0; } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

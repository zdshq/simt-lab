#include "simt/machine.hpp"
#include <charconv>
#include <iostream>
#include <string>
#include <string_view>

namespace {
struct Options { simt::Config config; std::string program; bool dump = false; bool help = false; };

// Parse decimal arguments without accepting trailing text or overflow.
// 解析十进制参数，拒绝尾随文本和数值溢出。
uint64_t number(std::string_view input) {
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size())
        throw std::runtime_error("invalid_integer_argument");
    return value;
}

// Escape error text before emitting machine-readable JSON.
// 输出机器可读 JSON 之前转义错误文本。
std::string escape(std::string_view text) {
    std::string result;
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') result += '\\';
        result += ch >= 32 ? static_cast<char>(ch) : '?';
    }
    return result;
}

// Select the deterministic PC-group policy.
// 选择确定性的 PC 线程组调度策略。
simt::Policy policy(std::string_view value) {
    if (value == "rr") return simt::Policy::round_robin;
    if (value == "min") return simt::Policy::min_pc;
    if (value == "max") return simt::Policy::max_pc;
    throw std::runtime_error("unknown_policy");
}

// Apply value-bearing options independently of argument traversal.
// 独立处理带值选项，不与参数遍历逻辑混合。
void set_option(Options& options, const std::string& key, const std::string& value) {
    auto& config = options.config;
    if (key == "--policy") { config.policy = policy(value); return; }
    if (key == "--max-steps") { config.max_steps = number(value); return; }
    if (key != "--sms" && key != "--blocks" && key != "--threads")
        throw std::runtime_error("unknown_option: " + key);
    const auto numeric = number(value);
    if (numeric > UINT32_MAX) throw std::runtime_error("argument_out_of_range");
    if (key == "--sms") config.sms = static_cast<uint32_t>(numeric);
    if (key == "--blocks") config.blocks = static_cast<uint32_t>(numeric);
    if (key == "--threads") config.threads = static_cast<uint32_t>(numeric);
}

// Collect launch options before constructing a machine.
// 构造模拟器之前收集启动选项。
Options parse_options(int argc, char** argv) {
    Options options;
    for (int at = 1; at < argc; ++at) {
        const std::string key = argv[at];
        if (key == "--help") { options.help = true; continue; }
        if (key == "--dump-state") { options.dump = true; continue; }
        if (key.starts_with("--")) {
            if (++at >= argc) throw std::runtime_error("missing_option_value");
            set_option(options, key, argv[at]);
        } else {
            if (!options.program.empty()) throw std::runtime_error("multiple_programs");
            options.program = key;
        }
    }
    if (!options.help && options.program.empty()) throw std::runtime_error("missing_program; use --help");
    return options;
}

// Describe the functional CLI without implying hardware timing support.
// 说明功能模拟器接口，不暗示支持硬件时序预测。
void print_help() {
    std::cout << "simt-run PROGRAM [--sms N] [--blocks N] [--threads N]\n"
                 "         [--policy rr|min|max] [--max-steps N] [--dump-state]\n"
                 "CPU functional simulator; instruction steps are not GPU cycles.\n";
}

// Coordinate loading, execution, and result reporting.
// 组织装载、执行和结果输出。
void run_simulation(const Options& options) {
    if (options.help) { print_help(); return; }
    simt::Machine machine(simt::Module::load(options.program), options.config);
    machine.run();
    machine.dump(std::cout, options.dump);
}

// Convert exceptions into JSON diagnostics and a process exit code.
// 将异常转换为 JSON 诊断信息和进程退出码。
int run_application(int argc, char** argv) {
    try {
        run_simulation(parse_options(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"status\":\"error\",\"error\":\"" << escape(error.what()) << "\"}\n";
        return 2;
    }
}
}

int main(int argc, char** argv) {
    return run_application(argc, argv);
}

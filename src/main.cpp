#include "simt/machine.hpp"
#include <charconv>
#include <iostream>
#include <string>
#include <string_view>

namespace {
struct Options {
    simt::Config config;
    std::string program, global, constant;
    bool dump = false, memory = false, help = false;
};

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
bool hardware_option(simt::Config& config, const std::string& key, uint32_t value) {
    if (key == "--shared-bytes") config.cta_shared_bytes = value;
    else if (key == "--partitions") config.pipeline.partitions = value;
    else if (key == "--register-banks") config.pipeline.register_banks = value;
    else if (key == "--collectors") config.pipeline.collectors = value;
    else if (key == "--max-ctas") config.pipeline.max_ctas = value;
    else if (key == "--max-warps") config.pipeline.max_warps = value;
    else if (key == "--sm-registers") config.pipeline.register_words = value;
    else if (key == "--int-latency") config.pipeline.integer_latency = value;
    else if (key == "--fp-latency") config.pipeline.float_latency = value;
    else if (key == "--sfu-latency") config.pipeline.special_latency = value;
    else if (key == "--sfu-interval") config.pipeline.special_interval = value;
    else return false;
    return true;
}

// Tensor geometry and port settings describe this research machine, not NVIDIA hardware constants.
// 张量几何结构和端口配置描述本研究机器，不是 NVIDIA 硬件常量。
bool tensor_option(simt::Config& config, const std::string& key, uint32_t value) {
    if (key == "--tensor-rows") config.tensor.rows = value;
    else if (key == "--tensor-columns") config.tensor.columns = value;
    else if (key == "--tensor-latency") config.tensor.latency = value;
    else if (key == "--tensor-queue") config.tensor.input_capacity = value;
    else if (key == "--tmem-banks") config.tensor.tmem_banks = value;
    else if (key == "--tmem-latency") config.tensor.tmem_latency = value;
    else if (key == "--tmem-columns") config.tensor.tmem_columns = value;
    else return false;
    return true;
}

// Expose finite DMA staging and transaction-table resources for controlled experiments.
// 暴露有限 DMA 暂存和事务表资源，以便进行受控实验。
bool async_option(simt::Config& config, const std::string& key, uint32_t value) {
    if (key == "--async-depth") config.async.commands = value;
    else if (key == "--async-fragments") config.async.fragments = value;
    else if (key == "--async-chunk") config.async.chunk_bytes = value;
    else if (key == "--mbarrier-slots") config.async.barrier_slots = value;
    else if (key == "--mbarrier-latency") config.async.barrier_latency = value;
    else if (key == "--atomic-banks") config.atomic.banks = value;
    else if (key == "--atomic-depth") config.atomic.capacity = value;
    else if (key == "--atomic-latency") config.atomic.latency = value;
    else return false;
    return true;
}

// Keep ordinary launch options independent from research hardware parameters.
// 将普通启动选项与研究型硬件参数分开。
void set_option(Options& options, const std::string& key, const std::string& value) {
    auto& config = options.config;
    if (key == "--policy") { config.policy = policy(value); return; }
    if (key == "--warp-policy") { config.warp_policy = policy(value); return; }
    if (key == "--global") { options.global = value; return; }
    if (key == "--constant") { options.constant = value; return; }
    if (key == "--max-steps") { config.max_steps = number(value); return; }
    if (key == "--max-cycles") { config.max_cycles = number(value); return; }
    const auto numeric = number(value);
    if (numeric > UINT32_MAX) throw std::runtime_error("argument_out_of_range");
    if (hardware_option(config, key, static_cast<uint32_t>(numeric))) return;
    if (tensor_option(config, key, static_cast<uint32_t>(numeric))) return;
    if (async_option(config, key, static_cast<uint32_t>(numeric))) return;
    if (key != "--sms" && key != "--blocks" && key != "--threads")
        throw std::runtime_error("unknown_option: " + key);
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
        if (key == "--dump-memory") { options.memory = true; continue; }
        if (key == "--timing") { options.config.timing = true; continue; }
        if (key == "--hardware-trace") { options.config.timing = true; options.config.hardware_trace = true; continue; }
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

// Research model clocks are distinct from functional steps and measured NVIDIA clocks.
// 研究模型时钟与功能步数、实测 NVIDIA 时钟不同。
void print_help() {
    std::cout << "simt-run PROGRAM [--sms N] [--blocks N] [--threads N]\n"
                 "         [--policy rr|min|max] [--warp-policy rr|min|max] [--max-steps N]\n"
                 "         [--global FILE] [--constant FILE] [--dump-state] [--dump-memory]\n"
                 "         [--timing] [--hardware-trace] [--max-cycles N] [--shared-bytes N]\n"
                 "         [--partitions N] [--register-banks N] [--collectors N] [--max-ctas N]\n"
                 "         [--max-warps N] [--sm-registers N] [--int-latency N] [--fp-latency N]\n"
                 "         [--sfu-latency N] [--sfu-interval N]\n"
                 "         [--tensor-rows N] [--tensor-columns N] [--tensor-latency N] [--tensor-queue N]\n"
                 "         [--tmem-banks N] [--tmem-latency N] [--tmem-columns N]\n"
                 "         [--async-depth N] [--async-fragments N] [--async-chunk N]\n"
                 "         [--mbarrier-slots N] [--mbarrier-latency N]\n"
                 "         [--atomic-banks N] [--atomic-depth N] [--atomic-latency N]\n"
                 "CPU simulator; --timing enables a partial research model, not NVIDIA timing prediction.\n";
}

// Coordinate loading, execution, and result reporting.
// 组织装载、执行和结果输出。
void run_simulation(const Options& options) {
    if (options.help) { print_help(); return; }
    simt::Machine machine(simt::Module::load(options.program), options.config);
    machine.load_images(options.global, options.constant);
    machine.run();
    machine.dump(std::cout, options.dump, options.memory);
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

#include "simt/protocol.hpp"
#include <iostream>

namespace {
// Errors belong to individual requests and do not silently destroy the persistent device.
// 错误归属于单个请求，不会静默销毁持久设备。
void respond(simt::Device& device, const std::string& line) {
    try {
        const auto result = simt::execute_request(device, line);
        device.publish_monitor();
        std::cout << "{\"ok\":true,\"result\":" << result << "}\n";
    } catch (const std::exception& error) {
        device.publish_monitor();
        std::cout << "{\"ok\":false,\"error\":" << simt::json_string(error.what()) << "}\n";
    }
    std::cout.flush();
}

// EOF or QUIT closes the process; the host chooses whether to drain work first.
// EOF 或 QUIT 关闭进程，是否先排空工作由主机决定。
void serve(simt::Device& device) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT") { std::cout << "{\"ok\":true,\"result\":null}\n" << std::flush; return; }
        respond(device, line);
    }
}

// Optional timing flags preserve the original two positional configuration arguments.
// 可选时序标志保留原来的两个位置配置参数。
struct TimingOptions { bool timing = false, trace = false, tensor_rtl = false; };
TimingOptions timing_options(int argc, char** argv) {
    TimingOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--timing") options.timing = true;
        else if (option == "--hardware-trace") options.trace = true;
        else if (option == "--tensor-rtl") options.tensor_rtl = true;
        else throw std::runtime_error("unknown_device_option: " + option);
    }
    if (options.trace && !options.timing) throw std::runtime_error("hardware_trace_requires_timing");
    return options;
}

// Parse device configuration before starting the persistent request loop.
// 启动持久请求循环之前解析设备配置。
void run_server(int argc, char** argv) {
    if (argc != 1 && argc < 3)
        throw std::runtime_error("usage: simt-device [SM_COUNT QUEUE_CAPACITY [--timing] [--hardware-trace] [--tensor-rtl]]");
    simt::RequestReader reader(argc == 1 ? "2 64" : std::string(argv[1]) + " " + argv[2]);
    const auto sms = reader.word(), capacity = reader.word(); reader.end();
    const auto options = timing_options(argc, argv);
    simt::Device device(sms, capacity, options.timing, options.trace, options.tensor_rtl);
    serve(device);
}

// Keep process-level failures separate from recoverable request errors.
// 区分进程级失败与可恢复的请求错误。
int run_application(int argc, char** argv) {
    try { run_server(argc, argv); return 0; }
    catch (const std::exception& error) {
        std::cerr << "{\"error\":" << simt::json_string(error.what()) << "}\n";
        return 2;
    }
}
}

int main(int argc, char** argv) { return run_application(argc, argv); }

#include "simt/protocol.hpp"
#include <iostream>

namespace {
// Errors belong to individual requests and do not silently destroy the persistent device.
// 错误归属于单个请求，不会静默销毁持久设备。
void respond(simt::Device& device, const std::string& line) {
    try {
        const auto result = simt::execute_request(device, line);
        std::cout << "{\"ok\":true,\"result\":" << result << "}\n";
    } catch (const std::exception& error) {
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

// Parse device configuration before starting the persistent request loop.
// 启动持久请求循环之前解析设备配置。
void run_server(int argc, char** argv) {
    if (argc != 1 && argc != 3) throw std::runtime_error("usage: simt-device [SM_COUNT QUEUE_CAPACITY]");
    simt::RequestReader reader(argc == 1 ? "2 64" : std::string(argv[1]) + " " + argv[2]);
    const auto sms = reader.word(), capacity = reader.word(); reader.end();
    simt::Device device(sms, capacity);
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

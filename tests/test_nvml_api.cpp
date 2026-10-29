#define NVML_NO_UNVERSIONED_FUNC_DEFS
#include <nvml.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

nvmlDevice_t initialize() {
    unsigned int count = 0;
    require(nvmlDeviceGetCount_v2(&count) == NVML_ERROR_UNINITIALIZED, "uninitialized count");
    require(nvmlInit_v2() == NVML_SUCCESS, "initialize");
    require(nvmlDeviceGetCount_v2(nullptr) == NVML_ERROR_INVALID_ARGUMENT, "null count");
    require(nvmlDeviceGetCount_v2(&count) == NVML_SUCCESS && count == 1, "live enumeration");
    nvmlDevice_t device{};
    require(nvmlDeviceGetHandleByIndex_v2(0, &device) == NVML_SUCCESS, "device handle");
    return device;
}

// ABI checks exercise the public structs, sizes, handle validation, and unsupported physical data.
// ABI 检查覆盖公开结构体、大小、句柄验证和未支持的物理数据。
void inspect(nvmlDevice_t device, uint64_t expected) {
    nvmlMemory_v2_t memory{}; memory.version = nvmlMemory_v2;
    require(nvmlDeviceGetMemoryInfo_v2(device, &memory) == NVML_SUCCESS, "memory v2");
    require(memory.total == (64ULL << 20) && memory.used == expected && !memory.reserved, "live allocation bytes");
    require(memory.free + memory.used == memory.total, "memory accounting");
    memory.version = 0;
    require(nvmlDeviceGetMemoryInfo_v2(device, &memory) == NVML_ERROR_ARGUMENT_VERSION_MISMATCH, "struct version");
    char name[2];
    require(nvmlDeviceGetName(device, name, sizeof(name)) == NVML_ERROR_INSUFFICIENT_SIZE, "short string buffer");
    require(nvmlDeviceGetUUID(nullptr, name, sizeof(name)) == NVML_ERROR_INVALID_ARGUMENT, "foreign handle");
    unsigned int watts = 123;
    require(nvmlDeviceGetPowerUsage(device, &watts) == NVML_ERROR_NOT_SUPPORTED && watts == 123, "no fake power");
}

// A handle retained across process death must fail rather than reading a stale snapshot.
// 跨越进程死亡保留的句柄必须失败，不能读取陈旧快照。
void wait_for_session_exit(nvmlDevice_t device) {
    std::cout << "ready" << std::endl;
    std::string resume;
    std::getline(std::cin, resume);
    nvmlMemory_t memory{};
    require(nvmlDeviceGetMemoryInfo(device, &memory) == NVML_ERROR_GPU_IS_LOST, "stale handle remained alive");
    unsigned int count = 1;
    require(nvmlDeviceGetCount_v2(&count) == NVML_SUCCESS && !count, "dead device still enumerated");
}

int run_application(int argc, char** argv) {
    try {
        const auto device = initialize();
        inspect(device, argc > 1 ? std::stoull(argv[1]) : 0);
        if (argc > 2) wait_for_session_exit(device);
        require(nvmlShutdown() == NVML_SUCCESS, "shutdown");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main(int argc, char** argv) { return run_application(argc, argv); }

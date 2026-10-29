#define NVML_NO_UNVERSIONED_FUNC_DEFS
#include <nvml.h>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void count_devices(unsigned int expected) {
    unsigned int count = 0;
    require(nvmlDeviceGetCount_v2(&count) == NVML_SUCCESS && count == expected, "merged device count");
}

nvmlDevice_t device_at(unsigned int index) {
    nvmlDevice_t device{};
    require(nvmlDeviceGetHandleByIndex_v2(index, &device) == NVML_SUCCESS, "merged device handle");
    unsigned int found = UINT32_MAX;
    require(nvmlDeviceGetIndex(device, &found) == NVML_SUCCESS && found == index, "merged device index");
    return device;
}

void verify_identity(nvmlDevice_t device) {
    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE]{};
    nvmlDevice_t duplicate{};
    require(nvmlDeviceGetUUID(device, uuid, sizeof(uuid)) == NVML_SUCCESS, "device UUID");
    require(nvmlDeviceGetHandleByUUID(uuid, &duplicate) == NVML_SUCCESS && duplicate == device, "stable UUID handle");
}

void verify_physical(nvmlDevice_t device) {
    nvmlPciInfo_t pci{};
    nvmlDevice_t duplicate{};
    require(nvmlDeviceGetPciInfo_v3(device, &pci) == NVML_SUCCESS, "native PCI query");
    require(nvmlDeviceGetHandleByPciBusId_v2(pci.busId, &duplicate) == NVML_SUCCESS && duplicate == device, "native PCI identity");
    nvmlMemory_v2_t memory{}; memory.version = nvmlMemory_v2;
    require(nvmlDeviceGetMemoryInfo_v2(device, &memory) == NVML_SUCCESS && memory.total > (64ULL<<20), "native memory query");
    unsigned int value = 0;
    require(nvmlDeviceIsMigDeviceHandle(device, &value) == NVML_SUCCESS, "native MIG classification");
}

// Simulator state and foreign pointers cannot enter the native public-query branch.
// 模拟器状态和外来指针不能进入原生公开查询分支。
void verify_simulated(nvmlDevice_t device, uint64_t expected) {
    nvmlMemory_v2_t memory{}; memory.version = nvmlMemory_v2;
    require(nvmlDeviceGetMemoryInfo_v2(device, &memory) == NVML_SUCCESS, "simulated memory query");
    require(memory.total == (64ULL<<20) && memory.used == expected, "simulated allocation bytes");
    unsigned int power = 123;
    require(nvmlDeviceGetPowerUsage(device, &power) == NVML_ERROR_NOT_SUPPORTED && power == 123, "simulated power unsupported");
    const auto foreign = reinterpret_cast<nvmlDevice_t>(uintptr_t{1});
    require(nvmlDeviceGetPowerUsage(foreign, &power) == NVML_ERROR_INVALID_ARGUMENT, "foreign native query handle");
    require(nvmlDeviceGetName(foreign, nullptr, 0) == NVML_ERROR_INVALID_ARGUMENT, "foreign common query handle");
}

void query_loop(nvmlDevice_t device, bool simulated) {
    for (unsigned int iteration = 0; iteration < 100; ++iteration) {
        char name[NVML_DEVICE_NAME_BUFFER_SIZE]{};
        require(nvmlDeviceGetName(device, name, sizeof(name)) == NVML_SUCCESS, "concurrent name query");
        require((std::string(name).find("(simulator)") != std::string::npos) == simulated, "cross-thread routing");
        nvmlMemory_t memory{};
        require(nvmlDeviceGetMemoryInfo(device, &memory) == NVML_SUCCESS, "concurrent memory query");
        require((memory.total == (64ULL<<20)) == simulated, "cross-thread memory owner");
    }
}

void verify_concurrent_queries(nvmlDevice_t physical, nvmlDevice_t simulated) {
    auto real = std::async(std::launch::async, [=] { query_loop(physical, false); });
    auto virtual_device = std::async(std::launch::async, [=] { query_loop(simulated, true); });
    real.get(); virtual_device.get();
}

void wait_for_exit(nvmlDevice_t physical, nvmlDevice_t simulated, unsigned int physical_count) {
    std::cout << "ready" << std::endl;
    std::string resume;
    std::getline(std::cin, resume);
    nvmlMemory_t memory{};
    require(nvmlDeviceGetMemoryInfo(simulated, &memory) == NVML_ERROR_GPU_IS_LOST, "dead simulator handle");
    count_devices(physical_count);
    verify_physical(physical);
}

void exercise(unsigned int physical_count, uint64_t allocated, bool wait) {
    require(nvmlInit_v2() == NVML_SUCCESS && nvmlInit_v2() == NVML_SUCCESS, "nested initialization");
    count_devices(physical_count+1);
    const auto physical = device_at(0), simulated = device_at(physical_count);
    verify_identity(physical); verify_identity(simulated);
    verify_physical(physical); verify_simulated(simulated, allocated);
    verify_concurrent_queries(physical, simulated);
    require(nvmlShutdown() == NVML_SUCCESS, "nested shutdown");
    count_devices(physical_count+1);
    if (wait) wait_for_exit(physical, simulated, physical_count);
    require(nvmlShutdown() == NVML_SUCCESS, "last shutdown");
    unsigned int count = 0;
    require(nvmlDeviceGetCount_v2(&count) == NVML_ERROR_UNINITIALIZED, "shutdown did not end session");
    require(nvmlInit_v2() == NVML_SUCCESS, "reinitialize native NVML");
    count_devices(physical_count+(wait ? 0 : 1));
    require(nvmlShutdown() == NVML_SUCCESS, "final shutdown");
}

int run_application(int argc, char** argv) {
    try {
        require(argc >= 3, "expected physical count and allocated bytes");
        exercise(std::stoul(argv[1]), std::stoull(argv[2]), argc > 3);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main(int argc, char** argv) { return run_application(argc, argv); }

#include "backend.hpp"
#include <cstdlib>
#include <dlfcn.h>

using namespace simt::nvml;

extern "C" {
nvmlReturn_t nvmlInit() { return initialize(); }
nvmlReturn_t nvmlInit_v2() { return initialize(); }
nvmlReturn_t nvmlInitWithFlags(unsigned int) { return initialize(); }
nvmlReturn_t nvmlShutdown() { return shutdown(); }
nvmlReturn_t nvmlDeviceGetCount(unsigned int* value) { return count(value); }
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* value) { return count(value); }
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int at, nvmlDevice_t* device) { return by_index(at, device); }
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int at, nvmlDevice_t* device) { return by_index(at, device); }
nvmlReturn_t nvmlDeviceGetHandleByUUID(const char* uuid, nvmlDevice_t* device) { return by_uuid(uuid, device); }
nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t device, unsigned int* value) { return index(device, value); }

// Names explicitly identify a simulated device and never claim an NVIDIA chip identity.
// 名称明确标识模拟设备，不声称是 NVIDIA 芯片。
nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char* value, unsigned int length) {
    return inspect(device, [&](const auto& record) {
        return copy_string(value, length, "SIMT Lab " + std::to_string(record.sms) + " SM (simulator)");
    });
}

nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char* value, unsigned int length) {
    return inspect(device, [&](const auto& record) { return copy_string(value, length, record.uuid); });
}

nvmlReturn_t nvmlSystemGetDriverVersion(char* value, unsigned int length) {
    return copy_string(value, length, "SIMT-1.0");
}

nvmlReturn_t nvmlSystemGetNVMLVersion(char* value, unsigned int length) {
    return copy_string(value, length, "SIMT-NVML-1.0");
}

// Allocation counters come from the live C++ device, including free and queued resource lifetimes.
// 分配计数来自存活 C++ 设备，包含释放和队列资源生命周期。
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t* memory) {
    if (!memory) return NVML_ERROR_INVALID_ARGUMENT;
    return inspect(device, [&](const auto& record) {
        memory->total = record.total; memory->used = record.used; memory->free = record.total - record.used;
        return NVML_SUCCESS;
    });
}

nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t* memory) {
    if (!memory) return NVML_ERROR_INVALID_ARGUMENT;
    if (memory->version != nvmlMemory_v2) return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    return inspect(device, [&](const auto& record) {
        memory->total = record.total; memory->used = record.used;
        memory->free = record.total - record.used; memory->reserved = 0;
        return NVML_SUCCESS;
    });
}

const char* nvmlErrorString(nvmlReturn_t error) {
    switch (error) {
    case NVML_SUCCESS: return "Success";
    case NVML_ERROR_UNINITIALIZED: return "Uninitialized";
    case NVML_ERROR_INVALID_ARGUMENT: return "Invalid argument";
    case NVML_ERROR_NOT_SUPPORTED: return "Not Supported";
    case NVML_ERROR_INSUFFICIENT_SIZE: return "Insufficient size";
    case NVML_ERROR_GPU_IS_LOST: return "Simulator session ended";
    case NVML_ERROR_NOT_FOUND: return "Not Found";
    default: return "SIMT NVML error";
    }
}

// The installed nvidia-smi requires its matching private export handshake before public NVML calls.
// 已安装 nvidia-smi 在调用公开 NVML 之前需要匹配的私有导出握手。
nvmlReturn_t nvmlInternalGetExportTable(const void** table, const void* id) {
    const auto* configured = std::getenv("SIMT_REAL_NVML");
    const auto* path = configured && *configured ? configured : SIMT_REAL_NVML_PATH;
    static void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) return NVML_ERROR_LIBRARY_NOT_FOUND;
    using Function = nvmlReturn_t (*)(const void**, const void*);
    const auto function = reinterpret_cast<Function>(dlsym(library, "nvmlInternalGetExportTable"));
    return function ? function(table, id) : NVML_ERROR_FUNCTION_NOT_FOUND;
}
}

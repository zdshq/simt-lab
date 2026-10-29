#include "backend.hpp"

using namespace simt::nvml;

extern "C" {
nvmlReturn_t nvmlInit() {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlInit)>("nvmlInit");
    return initialize();
}
nvmlReturn_t nvmlInit_v2() {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlInit_v2)>("nvmlInit_v2");
    return initialize();
}
nvmlReturn_t nvmlInitWithFlags(unsigned int flags) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlInitWithFlags)>("nvmlInitWithFlags", flags);
    return initialize(flags);
}
nvmlReturn_t nvmlShutdown() {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlShutdown)>("nvmlShutdown");
    return shutdown();
}
nvmlReturn_t nvmlDeviceGetCount(unsigned int* value) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetCount)>("nvmlDeviceGetCount", value);
    return count(value);
}
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* value) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetCount_v2)>("nvmlDeviceGetCount_v2", value);
    return count(value);
}
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int at, nvmlDevice_t* device) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetHandleByIndex)>("nvmlDeviceGetHandleByIndex", at, device);
    return by_index(at, device);
}
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int at, nvmlDevice_t* device) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetHandleByIndex_v2)>("nvmlDeviceGetHandleByIndex_v2", at, device);
    return by_index(at, device);
}
nvmlReturn_t nvmlDeviceGetHandleByUUID(const char* uuid, nvmlDevice_t* device) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetHandleByUUID)>("nvmlDeviceGetHandleByUUID", uuid, device);
    return by_uuid(uuid, device);
}
nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t device, unsigned int* value) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetIndex)>("nvmlDeviceGetIndex", device, value);
    return index(device, value);
}

nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char* bus, nvmlDevice_t* device) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetHandleByPciBusId_v2)>("nvmlDeviceGetHandleByPciBusId_v2", bus, device);

    if (!bus) return NVML_ERROR_INVALID_ARGUMENT;
    return native_lookup(device, [&](auto* native) {
        return native_call<decltype(&nvmlDeviceGetHandleByPciBusId_v2)>("nvmlDeviceGetHandleByPciBusId_v2", bus, native);
    });
}

nvmlReturn_t nvmlDeviceGetHandleByPciBusId(const char* bus, nvmlDevice_t* device) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetHandleByPciBusId)>("nvmlDeviceGetHandleByPciBusId", bus, device);

    return nvmlDeviceGetHandleByPciBusId_v2(bus, device);
}

nvmlReturn_t nvmlDeviceGetHandleBySerial(const char* serial, nvmlDevice_t* device) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetHandleBySerial)>("nvmlDeviceGetHandleBySerial", serial, device);

    if (!serial) return NVML_ERROR_INVALID_ARGUMENT;
    return native_lookup(device, [&](auto* native) {
        return native_call<decltype(&nvmlDeviceGetHandleBySerial)>("nvmlDeviceGetHandleBySerial", serial, native);
    });
}

// Names explicitly identify a simulated device and never claim an NVIDIA chip identity.
// 名称明确标识模拟设备，不声称是 NVIDIA 芯片。
nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char* value, unsigned int length) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetName)>("nvmlDeviceGetName", device, value, length);

    return dispatch(device, [&](auto native) {
        return native_call<decltype(&nvmlDeviceGetName)>("nvmlDeviceGetName", native, value, length);
    }, [&](const auto& record) {
        return copy_string(value, length, "SIMT Lab " + std::to_string(record.sms) + " SM (simulator)");
    });
}

nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char* value, unsigned int length) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetUUID)>("nvmlDeviceGetUUID", device, value, length);

    return dispatch(device, [&](auto native) {
        return native_call<decltype(&nvmlDeviceGetUUID)>("nvmlDeviceGetUUID", native, value, length);
    }, [&](const auto& record) { return copy_string(value, length, record.uuid); });
}

nvmlReturn_t nvmlSystemGetDriverVersion(char* value, unsigned int length) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlSystemGetDriverVersion)>("nvmlSystemGetDriverVersion", value, length);

    return system_query([&] {
        return native_call<decltype(&nvmlSystemGetDriverVersion)>("nvmlSystemGetDriverVersion", value, length);
    }, [&] { return copy_string(value, length, "SIMT-1.0"); });
}

nvmlReturn_t nvmlSystemGetNVMLVersion(char* value, unsigned int length) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlSystemGetNVMLVersion)>("nvmlSystemGetNVMLVersion", value, length);

    return system_query([&] {
        return native_call<decltype(&nvmlSystemGetNVMLVersion)>("nvmlSystemGetNVMLVersion", value, length);
    }, [&] { return copy_string(value, length, "SIMT-NVML-1.0"); });
}

// Allocation counters come from the live C++ device, including free and queued resource lifetimes.
// 分配计数来自存活 C++ 设备，包含释放和队列资源生命周期。
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t* memory) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetMemoryInfo)>("nvmlDeviceGetMemoryInfo", device, memory);

    if (!memory) return NVML_ERROR_INVALID_ARGUMENT;
    return dispatch(device, [&](auto native) {
        return native_call<decltype(&nvmlDeviceGetMemoryInfo)>("nvmlDeviceGetMemoryInfo", native, memory);
    }, [&](const auto& record) {
        memory->total = record.total; memory->used = record.used; memory->free = record.total - record.used;
        return NVML_SUCCESS;
    });
}

nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t* memory) {
    if (simt::nvml::inside_native_call())
        return simt::nvml::native_call<decltype(&nvmlDeviceGetMemoryInfo_v2)>("nvmlDeviceGetMemoryInfo_v2", device, memory);

    if (!memory) return NVML_ERROR_INVALID_ARGUMENT;
    if (memory->version != nvmlMemory_v2) return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    return dispatch(device, [&](auto native) {
        return native_call<decltype(&nvmlDeviceGetMemoryInfo_v2)>("nvmlDeviceGetMemoryInfo_v2", native, memory);
    }, [&](const auto& record) {
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
    case NVML_ERROR_GPU_IS_LOST: return "Device session ended";
    case NVML_ERROR_NOT_FOUND: return "Not Found";
    default: return "SIMT NVML error";
    }
}

}

#pragma once
#define NVML_NO_UNVERSIONED_FUNC_DEFS
#include <nvml.h>
#include "simt/monitor.hpp"
#include <functional>
#include <cstdio>
#include <cstdlib>

namespace simt::nvml {
nvmlReturn_t initialize(unsigned int flags = 0);
nvmlReturn_t shutdown();
nvmlReturn_t count(unsigned int* result);
nvmlReturn_t by_index(unsigned int index, nvmlDevice_t* device);
nvmlReturn_t by_uuid(const char* uuid, nvmlDevice_t* device);
nvmlReturn_t index(nvmlDevice_t device, unsigned int* result);
nvmlReturn_t inspect(nvmlDevice_t device, const std::function<nvmlReturn_t(const MonitorRecord&)>& action);
using NativeAction = std::function<nvmlReturn_t(nvmlDevice_t)>;
using SimulatedAction = std::function<nvmlReturn_t(const MonitorRecord&)>;
nvmlReturn_t dispatch(nvmlDevice_t device, const NativeAction& native, const SimulatedAction& simulated);
nvmlReturn_t native_query(nvmlDevice_t device, const NativeAction& action);
nvmlReturn_t native_lookup(nvmlDevice_t* result, const std::function<nvmlReturn_t(nvmlDevice_t*)>& action);
nvmlReturn_t system_query(const std::function<nvmlReturn_t()>& native,
                          const std::function<nvmlReturn_t()>& simulated);
nvmlReturn_t copy_string(char* destination, unsigned int length, const std::string& value);
void* native_library();
bool inside_native_call();
struct NativeCallScope {
    NativeCallScope();
    ~NativeCallScope();
};

// Resolve an exact official signature from the native library, without global symbol interposition.
// 从原生库解析精确的官方函数签名，不通过全局符号查找转发。
void* native_symbol(const char* name);
template<class Function, class... Arguments>
nvmlReturn_t native_call(const char* name, Arguments... arguments) {
    if (!native_library()) return NVML_ERROR_LIBRARY_NOT_FOUND;
    const auto function = reinterpret_cast<Function>(native_symbol(name));
    NativeCallScope scope;
    const auto status = function ? function(arguments...) : NVML_ERROR_FUNCTION_NOT_FOUND;
    if (std::getenv("SIMT_NVML_TRACE")) std::fprintf(stderr, "native: %s -> %d\n", name, int(status));
    return status;
}
}

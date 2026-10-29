#include "backend.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>

namespace simt::nvml {
namespace {
struct Handle { std::filesystem::path path; nvmlDevice_t native = nullptr; };
struct Registry {
    std::mutex mutex;
    unsigned int clients = 0;
    bool native_enabled = false;
    std::vector<std::unique_ptr<Handle>> handles;
};

Registry& registry() { static Registry instance; return instance; }

// Native private query tables require the original handle; public dispatch still validates its identity.
// 原生私有查询表需要原始句柄；公开分派仍按身份验证它。
nvmlDevice_t public_handle(Handle& handle) {
    return handle.native ? handle.native : reinterpret_cast<nvmlDevice_t>(&handle);
}

nvmlReturn_t physical_count(const Registry& state, unsigned int& count) {
    count = 0;
    return state.native_enabled ? native_call<decltype(&nvmlDeviceGetCount_v2)>("nvmlDeviceGetCount_v2", &count)
                                : NVML_SUCCESS;
}

// Only handles returned by the native library enter the physical side of the registry.
// 只有原生库返回的句柄才能进入注册表中的物理设备分支。
nvmlDevice_t wrap_native(Registry& state, nvmlDevice_t native) {
    for (const auto& handle : state.handles)
        if (handle->native == native && handle->path.empty()) return native;
    state.handles.push_back(std::make_unique<Handle>(Handle{{}, native}));
    return native;
}

// Handles remain stable while enumeration refreshes; dead sessions never retain a visible index.
// 枚举刷新时句柄保持稳定，死亡会话不保留可见索引。
std::vector<Handle*> refresh(Registry& state) {
    std::vector<Handle*> live;
    for (const auto& path : live_monitors()) {
        const auto found = std::find_if(state.handles.begin(), state.handles.end(),
                                       [&](const auto& handle) { return handle->path == path; });
        if (found == state.handles.end()) {
            state.handles.push_back(std::make_unique<Handle>(Handle{path}));
            live.push_back(state.handles.back().get());
        } else live.push_back(found->get());
    }
    return live;
}
}

nvmlReturn_t initialize(unsigned int flags) {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) {
        const auto* mode = std::getenv("SIMT_NVML_MERGE");
        const bool merged = mode && std::string_view(mode) == "1";
        if (merged) {
            const auto status = native_call<decltype(&nvmlInitWithFlags)>("nvmlInitWithFlags", flags);
            if (status != NVML_SUCCESS) return status;
        }
        state.native_enabled = merged;
    }
    ++state.clients;
    return NVML_SUCCESS;
}

nvmlReturn_t shutdown() {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    if (state.clients == 1 && state.native_enabled) {
        const auto status = native_call<decltype(&nvmlShutdown)>("nvmlShutdown");
        if (status != NVML_SUCCESS) return status;
        state.native_enabled = false;
    }
    --state.clients;
    return NVML_SUCCESS;
}

nvmlReturn_t count(unsigned int* result) {
    if (!result) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    unsigned int physical = 0;
    const auto status = physical_count(state, physical);
    if (status != NVML_SUCCESS) return status;
    *result = physical + static_cast<unsigned int>(refresh(state).size());
    return NVML_SUCCESS;
}

nvmlReturn_t by_index(unsigned int index, nvmlDevice_t* device) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    unsigned int physical = 0;
    auto status = physical_count(state, physical);
    if (status != NVML_SUCCESS) return status;
    if (index < physical) {
        nvmlDevice_t native{};
        status = native_call<decltype(&nvmlDeviceGetHandleByIndex_v2)>("nvmlDeviceGetHandleByIndex_v2", index, &native);
        if (status == NVML_SUCCESS) *device = wrap_native(state, native);
        return status;
    }
    index -= physical;
    const auto live = refresh(state);
    if (index >= live.size()) return NVML_ERROR_INVALID_ARGUMENT;
    *device = reinterpret_cast<nvmlDevice_t>(live[index]);
    return NVML_SUCCESS;
}

nvmlReturn_t by_uuid(const char* uuid, nvmlDevice_t* device) {
    if (!uuid || !device) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    for (auto handle : refresh(state)) {
        const auto record = read_monitor(handle->path);
        if (record && std::string(record->uuid) == uuid) {
            *device = reinterpret_cast<nvmlDevice_t>(handle);
            return NVML_SUCCESS;
        }
    }
    if (!state.native_enabled) return NVML_ERROR_NOT_FOUND;
    nvmlDevice_t native{};
    const auto status = native_call<decltype(&nvmlDeviceGetHandleByUUID)>("nvmlDeviceGetHandleByUUID", uuid, &native);
    if (status == NVML_SUCCESS) *device = wrap_native(state, native);
    return status;
}

nvmlReturn_t index(nvmlDevice_t device, unsigned int* result) {
    if (!result) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    for (const auto& handle : state.handles)
        if (public_handle(*handle) == device && handle->native)
            return state.native_enabled ? native_call<decltype(&nvmlDeviceGetIndex)>("nvmlDeviceGetIndex", handle->native, result)
                                        : NVML_ERROR_GPU_IS_LOST;
    unsigned int physical = 0;
    const auto status = physical_count(state, physical);
    if (status != NVML_SUCCESS) return status;
    const auto live = refresh(state);
    for (size_t index = 0; index < live.size(); ++index)
        if (reinterpret_cast<nvmlDevice_t>(live[index]) == device) {
            *result = physical + static_cast<unsigned int>(index);
            return NVML_SUCCESS;
        }
    return NVML_ERROR_GPU_IS_LOST;
}

// Validate an opaque pointer by identity before dereferencing it or reading session data.
// 在解引用或读取会话数据之前，先按身份验证不透明指针。
nvmlReturn_t dispatch(nvmlDevice_t device, const NativeAction& native, const SimulatedAction& simulated) {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    for (const auto& handle : state.handles) {
        if (public_handle(*handle) != device) continue;
        if (handle->native) return state.native_enabled ? native(handle->native) : NVML_ERROR_GPU_IS_LOST;
        const auto record = read_monitor(handle->path);
        return record ? simulated(*record) : NVML_ERROR_GPU_IS_LOST;
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}

nvmlReturn_t inspect(nvmlDevice_t device, const SimulatedAction& action) {
    return dispatch(device, [](auto) { return NVML_ERROR_NOT_SUPPORTED; }, action);
}

nvmlReturn_t native_query(nvmlDevice_t device, const NativeAction& action) {
    return dispatch(device, action, [](const auto&) { return NVML_ERROR_NOT_SUPPORTED; });
}

nvmlReturn_t native_lookup(nvmlDevice_t* result, const std::function<nvmlReturn_t(nvmlDevice_t*)>& action) {
    if (!result) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    if (!state.native_enabled) return NVML_ERROR_NOT_SUPPORTED;
    nvmlDevice_t native{};
    const auto status = action(&native);
    if (status == NVML_SUCCESS) *result = wrap_native(state, native);
    return status;
}

nvmlReturn_t system_query(const std::function<nvmlReturn_t()>& native,
                          const std::function<nvmlReturn_t()>& simulated) {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    return state.native_enabled ? native() : simulated();
}

nvmlReturn_t copy_string(char* destination, unsigned int length, const std::string& value) {
    if (!destination || !length) return NVML_ERROR_INVALID_ARGUMENT;
    if (length <= value.size()) return NVML_ERROR_INSUFFICIENT_SIZE;
    std::memcpy(destination, value.c_str(), value.size() + 1);
    return NVML_SUCCESS;
}
}

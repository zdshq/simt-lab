#include "backend.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

namespace simt::nvml {
namespace {
struct Handle { std::filesystem::path path; };
struct Registry {
    std::mutex mutex;
    unsigned int clients = 0;
    std::vector<std::unique_ptr<Handle>> handles;
};

Registry& registry() { static Registry instance; return instance; }

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

nvmlReturn_t initialize() {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    ++state.clients;
    return NVML_SUCCESS;
}

nvmlReturn_t shutdown() {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    --state.clients;
    return NVML_SUCCESS;
}

nvmlReturn_t count(unsigned int* result) {
    if (!result) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    *result = static_cast<unsigned int>(refresh(state).size());
    return NVML_SUCCESS;
}

nvmlReturn_t by_index(unsigned int index, nvmlDevice_t* device) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
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
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t index(nvmlDevice_t device, unsigned int* result) {
    if (!result) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    const auto live = refresh(state);
    for (size_t index = 0; index < live.size(); ++index)
        if (reinterpret_cast<nvmlDevice_t>(live[index]) == device) {
            *result = static_cast<unsigned int>(index);
            return NVML_SUCCESS;
        }
    return NVML_ERROR_GPU_IS_LOST;
}

// Validate an opaque pointer by identity before dereferencing it or reading session data.
// 在解引用或读取会话数据之前，先按身份验证不透明指针。
nvmlReturn_t inspect(nvmlDevice_t device, const std::function<nvmlReturn_t(const MonitorRecord&)>& action) {
    auto& state = registry(); std::lock_guard guard(state.mutex);
    if (!state.clients) return NVML_ERROR_UNINITIALIZED;
    for (const auto& handle : state.handles) {
        if (reinterpret_cast<nvmlDevice_t>(handle.get()) != device) continue;
        const auto record = read_monitor(handle->path);
        return record ? action(*record) : NVML_ERROR_GPU_IS_LOST;
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}

nvmlReturn_t copy_string(char* destination, unsigned int length, const std::string& value) {
    if (!destination || !length) return NVML_ERROR_INVALID_ARGUMENT;
    if (length <= value.size()) return NVML_ERROR_INSUFFICIENT_SIZE;
    std::memcpy(destination, value.c_str(), value.size() + 1);
    return NVML_SUCCESS;
}
}

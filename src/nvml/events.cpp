#include "backend.hpp"
#include <memory>
#include <mutex>
#include <set>

namespace {
struct EventSets {
    std::mutex mutex;
    std::set<std::unique_ptr<unsigned int>> handles;
};
EventSets& events() { static EventSets state; return state; }
}

extern "C" {
// Empty event sets support client setup without inventing hardware event sources.
// 空事件集合支持客户端初始化，不虚构硬件事件源。
nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t* output) {
    if (!output) return NVML_ERROR_INVALID_ARGUMENT;
    auto& state = events(); std::lock_guard guard(state.mutex);
    auto handle = std::make_unique<unsigned int>(0);
    *output = reinterpret_cast<nvmlEventSet_t>(handle.get());
    state.handles.insert(std::move(handle));
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlEventSetFree(nvmlEventSet_t event) {
    auto& state = events(); std::lock_guard guard(state.mutex);
    for (auto at = state.handles.begin(); at != state.handles.end(); ++at)
        if (reinterpret_cast<nvmlEventSet_t>(at->get()) == event) {
            state.handles.erase(at);
            return NVML_SUCCESS;
        }
    return NVML_ERROR_INVALID_ARGUMENT;
}

nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t device, unsigned long long* types) {
    if (!types) return NVML_ERROR_INVALID_ARGUMENT;
    return simt::nvml::inspect(device, [&](const auto&) { *types = 0; return NVML_SUCCESS; });
}
}

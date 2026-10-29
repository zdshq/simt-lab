#include "backend.hpp"
#include <cstdlib>
#include <dlfcn.h>

namespace simt::nvml {
namespace { thread_local unsigned int native_depth = 0; }

// Vendor-internal public calls must see only native state and must not reenter the registry lock.
// 厂商库内部的公开调用只能看到原生状态，不能重新进入注册表锁。
NativeCallScope::NativeCallScope() { ++native_depth; }
NativeCallScope::~NativeCallScope() { --native_depth; }
bool inside_native_call() { return native_depth != 0; }

// Keep the matched driver library alive for private tables and typed public query callbacks.
// 为私有导出表和有类型的公开查询回调保持匹配的驱动库存活。
void* native_library() {
    static void* library = [] {
        const auto* configured = std::getenv("SIMT_REAL_NVML");
        const auto* path = configured && *configured ? configured : SIMT_REAL_NVML_PATH;
        return dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }();
    return library;
}

void* native_symbol(const char* name) {
    const auto library = native_library();
    return library ? dlsym(library, name) : nullptr;
}
}

// The installed NVIDIA program requires its matched private export handshake before public calls.
// 已安装的 NVIDIA 程序在公开调用前需要匹配的私有导出握手。
extern "C" nvmlReturn_t nvmlInternalGetExportTable(const void** table, const void* id) {
    using Function = nvmlReturn_t (*)(const void**, const void*);
    return simt::nvml::native_call<Function>("nvmlInternalGetExportTable", table, id);
}

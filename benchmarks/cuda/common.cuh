#pragma once
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace calibration {
inline void check(cudaError_t status) {
    if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
inline uint32_t number(const char* value, uint32_t minimum, uint32_t maximum) {
    size_t end = 0;
    const auto parsed = std::stoull(value,&end);
    if (value[0] == '-' || value[end] || parsed < minimum || parsed > maximum)
        throw std::runtime_error("invalid numeric argument");
    return static_cast<uint32_t>(parsed);
}

// Device allocations own their payload; copying results synchronizes before host validation.
// 设备分配持有自己的载荷；结果复制在宿主验证前完成同步。
template<class T> class Buffer {
public:
    T* data = nullptr;
    explicit Buffer(size_t count) { check(cudaMalloc(&data,count*sizeof(T))); }
    ~Buffer() { if (data) cudaFree(data); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    void upload(const std::vector<T>& values) { check(cudaMemcpy(data,values.data(),values.size()*sizeof(T),cudaMemcpyHostToDevice)); }
    std::vector<T> download(size_t count) {
        std::vector<T> values(count); check(cudaMemcpy(values.data(),data,count*sizeof(T),cudaMemcpyDeviceToHost)); return values;
    }
};
struct Sample { unsigned long long cycles; uint32_t value, sm; };

__device__ __forceinline__ unsigned long long clock_now() {
    unsigned long long result; asm volatile("mov.u64 %0, %%clock64;" : "=l"(result) :: "memory"); return result;
}
// Predicate the stop clock on the final loaded value, forcing its scoreboard dependency to resolve first.
// 结束时钟以最终加载值为谓词依赖，强制先解决该值的 scoreboard 依赖。
__device__ __forceinline__ unsigned long long clock_after(uint32_t value) {
    unsigned long long result;
    asm volatile("{ .reg .pred ready; setp.ne.u32 ready, %1, 0xffffffff; @ready mov.u64 %0, %%clock64; @!ready trap; }"
                 : "=l"(result) : "r"(value) : "memory");
    return result;
}
__device__ __forceinline__ uint32_t sm_id() {
    uint32_t result; asm volatile("mov.u32 %0, %%smid;" : "=r"(result)); return result;
}
template<bool GlobalOnly> __device__ __forceinline__ uint32_t global_load(const uint32_t* address) {
    uint32_t value;
    if constexpr (GlobalOnly) asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(value) : "l"(address) : "memory");
    else asm volatile("ld.global.ca.u32 %0, [%1];" : "=r"(value) : "l"(address) : "memory");
    return value;
}

inline void metadata() {
    cudaDeviceProp p{}; int runtime = 0, driver = 0, device = 0; char pci[32]{};
    check(cudaGetDevice(&device)); check(cudaGetDeviceProperties(&p,device));
    check(cudaRuntimeGetVersion(&runtime)); check(cudaDriverGetVersion(&driver));
    check(cudaDeviceGetPCIBusId(pci,sizeof(pci),device));
    std::cout << "{\"kind\":\"device\",\"name\":" << std::quoted(p.name) << ",\"pci_bus_id\":" << std::quoted(pci)
              << ",\"compute_capability\":" << std::quoted(std::to_string(p.major)+"."+std::to_string(p.minor))
              << ",\"device_ordinal\":" << device << ",\"sms\":" << p.multiProcessorCount << ",\"l2_bytes\":" << p.l2CacheSize
              << ",\"shared_per_sm\":" << p.sharedMemPerMultiprocessor << ",\"registers_per_sm\":" << p.regsPerMultiprocessor
              << ",\"global_bytes\":" << p.totalGlobalMem << ",\"runtime_version\":" << runtime << ",\"driver_api_version\":" << driver << "}\n";
}
inline void sample(const std::string& fields, uint32_t repeat, const Sample& value, float event_ms, bool clocked = true) {
    std::cout << "{\"kind\":\"sample\"," << fields << ",\"repeat\":" << repeat
              << ",\"cycles\":" << (clocked ? std::to_string(value.cycles) : "null")
              << ",\"sm\":" << (clocked ? std::to_string(value.sm) : "null")
              << ",\"final_value\":" << value.value << ",\"event_ms\":" << event_ms
              << ",\"check_passed\":true}\n" << std::flush;
}

class Events {
public:
    Events() { check(cudaEventCreate(&first_)); check(cudaEventCreate(&last_)); }
    ~Events() { cudaEventDestroy(first_); cudaEventDestroy(last_); }
    void begin() { check(cudaEventRecord(first_)); }
    float finish() {
        check(cudaGetLastError()); check(cudaEventRecord(last_)); check(cudaEventSynchronize(last_));
        float ms = 0; check(cudaEventElapsedTime(&ms,first_,last_)); return ms;
    }
private:
    cudaEvent_t first_{},last_{};
};
}

#pragma once
#include "common.cuh"

namespace calibration {
// Each lane follows a two-node shared-memory cycle; the next load address depends on the previous result.
// 每个 lane 沿 shared 内存中的双节点环前进；下一次加载地址依赖上次结果。
__global__ void shared_kernel(uint32_t stride, uint32_t iterations, uint32_t start_mask, Sample* output) {
    __shared__ uint32_t table[4096];
    const auto lane = threadIdx.x;
    for (uint32_t i = lane; i < 4096; i += 32) table[i] = i^2048;
    __syncthreads();
    uint32_t at = lane*stride;
    const auto started = clock_now(); at += static_cast<uint32_t>(started)&start_mask;
    #pragma unroll 1
    for (uint32_t i = 0; i < iterations; ++i) {
        const auto address = static_cast<uint32_t>(__cvta_generic_to_shared(table+at));
        asm volatile("ld.shared.u32 %0, [%1];" : "=r"(at) : "r"(address) : "memory");
    }
    const auto stopped = clock_after(at);
    output[lane] = {stopped-started,at,sm_id()};
}
inline void run_shared(int argc, char** argv) {
    if (argc != 6) throw std::runtime_error("shared stride iterations repeats device");
    const auto stride = number(argv[2],0,33), iterations = number(argv[3],0,1048576), repeats = number(argv[4],1,99);
    Buffer<Sample> output(32); Events events;
    shared_kernel<<<1,32>>>(stride,128,0,output.data); check(cudaDeviceSynchronize());
    const std::string fields = "\"experiment\":\"shared\",\"stride_words\":"+std::to_string(stride)+
                              ",\"iterations\":"+std::to_string(iterations);
    for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
        events.begin(); shared_kernel<<<1,32>>>(stride,iterations,0,output.data);
        const auto ms = events.finish(); const auto values = output.download(32);
        for (uint32_t lane = 0; lane < 32; ++lane)
            if (values[lane].value != ((lane*stride)^((iterations&1)*2048))) throw std::runtime_error("shared result mismatch");
        sample(fields,repeat,values[0],ms);
    }
}
}

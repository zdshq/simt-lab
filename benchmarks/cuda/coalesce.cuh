#pragma once
#include "common.cuh"

namespace calibration {
// Independent blocks access disjoint 2 KiB rows; output stores retain every lane's arithmetic dependency.
// 独立 block 访问互不重叠的 2 KiB 行；输出写入保留每个 lane 的算术依赖。
__global__ void coalesce_kernel(const uint32_t* data, uint32_t stride, uint32_t offset,
    uint32_t rows, uint32_t iterations, uint32_t* output) {
    const uint32_t lane = threadIdx.x;
    uint32_t sum = 0;
    #pragma unroll 1
    for (uint32_t i = 0; i < iterations; ++i) {
        const uint32_t row = blockIdx.x*rows+(i%rows);
        sum += global_load<true>(data+row*512+lane*stride+offset);
    }
    output[blockIdx.x*32+lane] = sum;
}
inline std::vector<uint32_t> coalesce_data(uint32_t words) {
    std::vector<uint32_t> data(words);
    for (uint32_t i = 0; i < words; ++i) data[i] = i*1664525u+1013904223u;
    return data;
}
inline void validate_coalesce(const std::vector<uint32_t>& values, uint32_t stride, uint32_t offset,
                             uint32_t rows, uint32_t iterations) {
    for (uint32_t at = 0; at < values.size(); ++at) {
        uint32_t expected = 0;
        for (uint32_t i = 0; i < iterations; ++i) {
            const auto word = ((at/32)*rows+i%rows)*512+(at%32)*stride+offset;
            expected += word*1664525u+1013904223u;
        }
        if (values[at] != expected) throw std::runtime_error("coalescing result mismatch");
    }
}
inline void run_coalesce(int argc, char** argv) {
    if (argc != 9) throw std::runtime_error("coalesce stride offset blocks rows iterations repeats device");
    const auto stride = number(argv[2],0,8), offset = number(argv[3],0,7), blocks = number(argv[4],1,4096);
    const auto rows = number(argv[5],1,256), iterations = number(argv[6],1,65536), repeats = number(argv[7],1,99);
    Buffer<uint32_t> data(size_t{blocks}*rows*512); data.upload(coalesce_data(blocks*rows*512));
    Buffer<uint32_t> output(blocks*32); Events events; double total = 0;
    coalesce_kernel<<<blocks,32>>>(data.data,stride,offset,rows,iterations,output.data); check(cudaDeviceSynchronize());
    const auto fields = "\"experiment\":\"coalesce\",\"stride_words\":"+std::to_string(stride)+",\"offset_words\":"+std::to_string(offset)+
        ",\"blocks\":"+std::to_string(blocks)+",\"rows\":"+std::to_string(rows)+",\"iterations\":"+std::to_string(iterations);
    for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
        events.begin(); coalesce_kernel<<<blocks,32>>>(data.data,stride,offset,rows,iterations,output.data);
        const float ms = events.finish(); const auto values = output.download(blocks*32);
        validate_coalesce(values,stride,offset,rows,iterations); total += ms;
        sample(fields,repeat,{0,values[0],0},ms,false);
    }
    std::cout << "KERNEL_PERF_RESULT {\"time_ms\":" << total/repeats << ",\"check_passed\":true,\"useful_bytes\":"
              << uint64_t{blocks}*32*iterations*4 << "}\n";
}
}

#pragma once
#include "common.cuh"
#include "../memory_pattern.hpp"

namespace calibration {
struct ChaseOptions { uint32_t bytes, iterations, repeats, seed, carveout; bool global_only; uint32_t group_bytes = 0; };

template<bool GlobalOnly> __global__ void chase_kernel(const uint32_t* data, uint32_t nodes,
    uint32_t iterations, uint32_t start_mask, Sample* result) {
    if (threadIdx.x != 0) return;
    uint32_t at = 0;
    #pragma unroll 1
    for (uint32_t i = 0; i < nodes; ++i) at = global_load<GlobalOnly>(data+at);
    const auto started = clock_after(at);
    at += static_cast<uint32_t>(started)&start_mask;
    #pragma unroll 1
    for (uint32_t i = 0; i < iterations; ++i) at = global_load<GlobalOnly>(data+at);
    const auto stopped = clock_after(at);
    result[0] = {stopped-started,at,sm_id()};
}

template<bool GlobalOnly> void chase_samples(const ChaseOptions& o) {
    const auto values = ring(o.bytes,o.seed,o.group_bytes); const auto expected = follow(values,o.iterations);
    Buffer<uint32_t> data(values.size()); data.upload(values); Buffer<Sample> output(1); Events events;
    check(cudaFuncSetAttribute(chase_kernel<GlobalOnly>,cudaFuncAttributePreferredSharedMemoryCarveout,o.carveout));
    const std::string fields = "\"experiment\":\"chase\",\"policy\":\""+std::string(GlobalOnly?"cg":"ca")+
        "\",\"bytes\":"+std::to_string(o.bytes)+",\"iterations\":"+std::to_string(o.iterations)+
        ",\"warm_loads\":"+std::to_string(o.bytes/32)+",\"seed\":"+std::to_string(o.seed)+",\"carveout_preference\":"+std::to_string(o.carveout)+
        ",\"locality_group_bytes\":"+std::to_string(o.group_bytes);
    for (uint32_t repeat = 0; repeat < o.repeats; ++repeat) {
        events.begin(); chase_kernel<GlobalOnly><<<1,32>>>(data.data,o.bytes/32,o.iterations,0,output.data);
        const float ms = events.finish(); const auto value = output.download(1)[0];
        if (value.value != expected) throw std::runtime_error("pointer chase result mismatch");
        sample(fields,repeat,value,ms);
    }
}
inline void run_chase(int argc, char** argv) {
    if (argc != 9 && argc != 10) throw std::runtime_error("chase ca|cg bytes iterations repeats seed carveout [locality_group_bytes] device");
    const std::string policy = argv[2];
    if (policy != "ca" && policy != "cg") throw std::runtime_error("invalid cache policy");
    ChaseOptions o{number(argv[3],1024,268435456),number(argv[4],0,1048576),number(argv[5],1,99),
                         number(argv[6],1,UINT32_MAX),number(argv[7],0,100),policy=="cg"};
    if (o.bytes%32) throw std::runtime_error("working set must contain whole sectors");
    if (argc == 10) o.group_bytes = number(argv[8],0,o.bytes);
    if (o.group_bytes && (o.group_bytes%32 || o.bytes%o.group_bytes)) throw std::runtime_error("invalid locality group");
    if (o.global_only) chase_samples<true>(o); else chase_samples<false>(o);
}
}

#pragma once
#include <array>
#include <cstdint>

namespace transaction_reference {
using Record=std::array<uint32_t,5>;
struct Result { Record record; uint32_t error=0; bool advanced=false; };

// Wide host arithmetic is the independent oracle for bounded counters and phase rollover rejection.
// 宽宿主算术作为有界计数和阶段回绕拒绝的独立参考。
inline Result update(Record old,uint32_t operation,uint32_t amount,uint32_t completion_phase) {
    Result r{old}; auto& n=r.record;
    switch (operation) {
    case 0: if (!amount || amount>1024) r.error=3; else n={0,amount,amount,0,0}; break;
    case 1: if (!amount || uint64_t{old[3]}+amount>UINT32_MAX) r.error=7; else n[3]+=amount; break;
    case 2: if (!old[2]) r.error=8; else --n[2]; break;
    case 3:
        if (!amount || amount>4096) r.error=9;
        else if (uint64_t{old[4]}+amount>old[3]) r.error=10;
        else n[4]+=amount;
        break;
    case 4:
        if (completion_phase!=old[0]) r.error=11;
        else if (!amount || amount>old[3] || amount>old[4]) r.error=12;
        else { n[3]-=amount; n[4]-=amount; }
        break;
    default: r.error=1;
    }
    if (!r.error && !n[2] && !n[3]) {
        if (n[0]==UINT32_MAX) r.error=13;
        else { ++n[0]; n[2]=n[1]; r.advanced=true; }
    }
    if (r.error) n=old;
    return r;
}
}

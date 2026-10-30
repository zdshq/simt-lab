#include "Vtransaction_update.h"
#include "bench.hpp"
#include "transaction_reference.hpp"
#include <random>

namespace {
using transaction_reference::Record;
void check(Vtransaction_update& dut,Record record,uint32_t op,uint32_t amount,uint32_t phase) {
    for (uint32_t i=0; i<5; ++i) dut.previous[4-i]=record[i];
    dut.operation=op; dut.amount=amount; dut.completion_phase=phase; dut.eval();
    const auto expected=transaction_reference::update(record,op,amount,phase);
    require(dut.error==expected.error && bool(dut.advanced)==expected.advanced,"transaction arithmetic status differs");
    for (uint32_t i=0; i<5; ++i) require(dut.updated[4-i]==expected.record[i],"transaction arithmetic record differs");
}
void boundaries() {
    Vtransaction_update dut;
    for (uint32_t phase : {0u,1u,UINT32_MAX-1,UINT32_MAX})
        for (uint32_t count : {0u,1u,1024u,1025u,UINT32_MAX})
            for (uint32_t bytes : {0u,1u,4096u,UINT32_MAX})
                for (uint32_t op=0; op<8; ++op) {
                    check(dut,{phase,1,0,bytes,bytes},op,count,phase);
                    check(dut,{phase,1024,1,bytes,0},op,count,phase+1);
                }
    dut.final();
}
void randomized() {
    Vtransaction_update dut; std::mt19937 rng(823911);
    for (uint32_t i=0; i<20000; ++i) {
        const uint32_t expected=1+rng()%1024, bytes=rng(), issued=bytes ? rng()%bytes : 0;
        const uint32_t phase=rng();
        check(dut,{phase,expected,uint32_t(rng()%(expected+1)),bytes,issued},rng()%8,rng(),i%2 ? phase : uint32_t(rng()));
    }
    dut.final();
}
int run_application() {
    try { run_case("transaction arithmetic boundaries, errors and atomic rejection",boundaries);
        run_case("transaction arithmetic 20000 independent wide-counter comparisons",randomized); return 0; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
}
int main() { return run_application(); }

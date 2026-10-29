#include "simt/register_file.hpp"
#include <stdexcept>

namespace simt::hw {
bool register_rtl_available() {
#ifdef SIMT_HAS_REGISTER_RTL
    return true;
#else
    return false;
#endif
}

// An unavailable data path must fail before executing a kernel, without a software fallback.
// 不可用的数据通路必须在执行 kernel 前失败，不回退到软件。
std::unique_ptr<RegisterFile> make_register_file(uint32_t partitions, uint32_t banks) {
#ifdef SIMT_HAS_REGISTER_RTL
    return make_rtl_register_file(partitions, banks);
#else
    static_cast<void>(partitions); static_cast<void>(banks);
    throw std::runtime_error("register_rtl_backend_unavailable; build with SIMT_REGISTER_RTL=ON");
#endif
}
}

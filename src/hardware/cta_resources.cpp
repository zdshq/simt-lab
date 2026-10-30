#include "simt/cta_resources.hpp"
#include <stdexcept>

#ifndef SIMT_HAS_REGISTER_RTL
namespace simt::hw {
std::unique_ptr<CtaResources> make_cta_resources(const PipelineConfig&) {
    throw std::runtime_error("cta_resource_rtl_not_built");
}
}
#endif

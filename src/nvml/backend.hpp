#pragma once
#define NVML_NO_UNVERSIONED_FUNC_DEFS
#include <nvml.h>
#include "simt/monitor.hpp"
#include <functional>

namespace simt::nvml {
nvmlReturn_t initialize();
nvmlReturn_t shutdown();
nvmlReturn_t count(unsigned int* result);
nvmlReturn_t by_index(unsigned int index, nvmlDevice_t* device);
nvmlReturn_t by_uuid(const char* uuid, nvmlDevice_t* device);
nvmlReturn_t index(nvmlDevice_t device, unsigned int* result);
nvmlReturn_t inspect(nvmlDevice_t device, const std::function<nvmlReturn_t(const MonitorRecord&)>& action);
nvmlReturn_t copy_string(char* destination, unsigned int length, const std::string& value);
}

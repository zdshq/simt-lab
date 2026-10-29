#pragma once
#include "simt/device.hpp"
#include <sstream>

namespace simt {
class RequestReader {
public:
    explicit RequestReader(const std::string& line) : input_(line) {}
    std::string token();
    uint64_t number();
    uint32_t word();
    uint32_t count(uint32_t maximum);
    void end();
private:
    std::istringstream input_;
};
Command read_command(RequestReader& reader);
std::string execute_request(Device& device, const std::string& line);
}

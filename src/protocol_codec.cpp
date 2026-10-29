#include "simt/protocol.hpp"
#include <charconv>
#include <iomanip>
#include <stdexcept>

namespace simt {
// Escape protocol strings without trusting exception messages to be valid JSON.
// 转义协议字符串，不假设异常消息天然符合 JSON 格式。
std::string json_string(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') output << '\\' << ch;
        else if (ch < 32) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned{ch};
        else output << ch;
    }
    output << '"';
    return output.str();
}

// Binary payloads use hex so line boundaries and whitespace cannot alter their contents.
// 二进制载荷使用十六进制，避免换行与空白改变内容。
std::string encode_hex(const std::vector<uint8_t>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto value : bytes) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}

// Empty payloads have an explicit token; malformed hex never reaches command execution.
// 空载荷有专门标记；畸形十六进制不会进入命令执行阶段。
std::vector<uint8_t> decode_hex(const std::string& text) {
    if (text == "-") return {};
    if (text.size() % 2 || text.size() > (128 << 20)) throw std::runtime_error("invalid_hex_payload");
    std::vector<uint8_t> bytes;
    for (size_t index = 0; index < text.size(); index += 2) {
        uint32_t value = 0;
        const auto* begin = text.data() + index;
        const auto [end, error] = std::from_chars(begin, begin + 2, value, 16);
        if (error != std::errc{} || end != begin + 2) throw std::runtime_error("invalid_hex_payload");
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}

std::string RequestReader::token() {
    std::string value;
    if (!(input_ >> value)) throw std::runtime_error("missing_protocol_argument");
    return value;
}

// Reject signs, trailing characters, and integer overflow at the process boundary.
// 在进程边界拒绝符号、尾随字符与整数溢出。
uint64_t RequestReader::number() {
    const auto text = token();
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid_protocol_integer");
    return value;
}

uint32_t RequestReader::word() {
    const auto value = number();
    if (value > UINT32_MAX) throw std::runtime_error("protocol_integer_out_of_range");
    return static_cast<uint32_t>(value);
}

uint32_t RequestReader::count(uint32_t maximum) {
    const auto value = word();
    if (value > maximum) throw std::runtime_error("protocol_count_out_of_range");
    return value;
}

// No mutation is allowed until the request has been parsed completely.
// 请求完整解析之前不允许修改设备状态。
void RequestReader::end() {
    std::string extra;
    if (input_ >> extra) throw std::runtime_error("unexpected_protocol_argument");
}
}

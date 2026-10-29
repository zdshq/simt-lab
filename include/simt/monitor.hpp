#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace simt {
struct MonitorRecord {
    uint64_t magic = 0x53494D544D4F4E31, version = 1;
    uint32_t pid = 0, sms = 0;
    uint64_t total = 64ULL << 20, used = 0, ticks = 0, completed = 0;
    uint64_t compute = 0, copy = 0;
    char uuid[80]{};
};

// A separate lifetime lock prevents stale files or reused PIDs from appearing as live devices.
// 独立生命周期锁防止陈旧文件或复用的 PID 被显示为存活设备。
class MonitorPublisher {
public:
    explicit MonitorPublisher(const std::string& directory);
    ~MonitorPublisher();
    MonitorPublisher(const MonitorPublisher&) = delete;
    MonitorPublisher& operator=(const MonitorPublisher&) = delete;
    void publish(MonitorRecord record);
private:
    std::filesystem::path state_path_, lock_path_;
    int state_fd_ = -1, lock_fd_ = -1;
    std::string uuid_;
    void create_files(const std::filesystem::path& directory);
    void cleanup();
};

std::filesystem::path monitor_directory();
std::optional<MonitorRecord> read_monitor(const std::filesystem::path& path);
std::vector<std::filesystem::path> live_monitors();
}

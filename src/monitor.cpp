#include "simt/monitor.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace simt {
namespace {
// File descriptors and lock release remain local even when a reader rejects malformed state.
// 即使读取者拒绝格式错误的状态，文件描述符和锁也在局部释放。
struct File {
    int fd;
    ~File() { if (fd >= 0) close(fd); }
};

std::string identity() {
    std::random_device random;
    char value[80];
    std::snprintf(value, sizeof(value), "GPU-%08x-%04x-%04x-%04x-%04x%08x", random(),
                  random() & 0xFFFF, random() & 0xFFFF, random() & 0xFFFF, random() & 0xFFFF, random());
    return value;
}

// The registry contains only this user's opt-in simulation sessions.
// 注册目录只包含此用户主动启用的模拟会话。
void prepare_directory(const std::filesystem::path& directory) {
    std::error_code error;
    const auto created = std::filesystem::create_directories(directory, error);
    if (error) throw std::runtime_error("cannot_create_monitor_directory");
    if (created && chmod(directory.c_str(), 0700)) throw std::runtime_error("cannot_protect_monitor_directory");
    struct stat info{};
    if (lstat(directory.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077))
        throw std::runtime_error("monitor_directory_must_be_owned_private_directory");
}
}

std::filesystem::path monitor_directory() {
    const auto* configured = std::getenv("SIMT_MONITOR_DIR");
    if (configured && *configured) return configured;
    return std::filesystem::path("/tmp") / ("simt-lab-" + std::to_string(getuid()));
}

MonitorPublisher::MonitorPublisher(const std::string& directory) : uuid_(identity()) {
    prepare_directory(directory);
    try { create_files(directory); }
    catch (...) { cleanup(); throw; }
}

void MonitorPublisher::create_files(const std::filesystem::path& directory) {
    const auto base = std::to_string(getpid()) + "-" + uuid_;
    lock_path_ = directory / (base + ".lock");
    state_path_ = directory / (base + ".state");
    lock_fd_ = open(lock_path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB)) throw std::runtime_error("cannot_lock_monitor_session");
    state_fd_ = open(state_path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (state_fd_ < 0) throw std::runtime_error("cannot_create_monitor_state");
}

// Per-update locking makes readers observe a complete snapshot, including allocation changes.
// 每次更新加锁，使读取者观察到完整快照，包括分配变化。
void MonitorPublisher::publish(MonitorRecord record) {
    record.pid = static_cast<uint32_t>(getpid());
    std::snprintf(record.uuid, sizeof(record.uuid), "%s", uuid_.c_str());
    if (flock(state_fd_, LOCK_EX)) throw std::runtime_error("monitor_write_lock_failed");
    const auto count = pwrite(state_fd_, &record, sizeof(record), 0);
    flock(state_fd_, LOCK_UN);
    if (count != sizeof(record)) throw std::runtime_error("monitor_write_failed");
}

void MonitorPublisher::cleanup() {
    if (state_fd_ >= 0) { unlink(state_path_.c_str()); close(state_fd_); state_fd_ = -1; }
    if (lock_fd_ >= 0) { unlink(lock_path_.c_str()); close(lock_fd_); lock_fd_ = -1; }
}

MonitorPublisher::~MonitorPublisher() { cleanup(); }

// A readable snapshot without a currently held lifetime lock is never a simulated GPU.
// 可读快照如果没有当前持有的生命周期锁，就不是存活的模拟 GPU。
std::optional<MonitorRecord> read_monitor(const std::filesystem::path& path) {
    auto lock_path = path; lock_path.replace_extension(".lock");
    File lifetime{open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
    if (lifetime.fd < 0) return {};
    if (flock(lifetime.fd, LOCK_EX | LOCK_NB) == 0 || errno != EWOULDBLOCK) return {};
    File state{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (state.fd < 0 || flock(state.fd, LOCK_SH)) return {};
    MonitorRecord record;
    const auto count = pread(state.fd, &record, sizeof(record), 0);
    if (count != sizeof(record) || record.magic != 0x53494D544D4F4E31 || record.version != 1 ||
        record.used > record.total || !record.pid || !record.sms || record.uuid[79]) return {};
    return record;
}

std::vector<std::filesystem::path> live_monitors() {
    std::vector<std::filesystem::path> paths;
    const auto directory = monitor_directory();
    std::error_code error;
    std::filesystem::directory_iterator it(directory, error);
    if (error) return paths;
    for (const auto& entry : it)
        if (entry.path().extension() == ".state" && read_monitor(entry.path())) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    return paths;
}
}

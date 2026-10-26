// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.physical_session
Purpose: Temporarily hand one identified controller to a bounded, unprivileged VM.
Created: 2026-10-25
Architecture: M11 host fixture. No raw media writes, formatting or shared-controller
handoff. A local administrator authenticates this exact operation through pkexec.
*/
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "sha256.hpp"
#include <fcntl.h>
#include <grp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
constexpr const char *bdf = "0000:13:00.3";
constexpr const char *device = "/sys/bus/pci/devices/0000:13:00.3";
constexpr const char *group_node = "/dev/vfio/37";
constexpr const char *serial =
    "0901cf2e59c6d7970b7ac6d39d38fdbb422cb705dd77fb3f46a8f23b3b771cc54ca3000000000000000000006421e64f009a58206755810742af79b9";
constexpr uid_t caller_uid = 1000;
constexpr gid_t caller_gid = 1000;
// Only signal-safe state is shared with signal handlers.
volatile std::sig_atomic_t interrupted{};
void signal_stop(int) { interrupted = 1; }
std::string read(const fs::path &path) {
    std::ifstream input(path);
    std::string text;
    std::getline(input, text);
    return text.size() <= 4096 ? text : std::string{};
}
fs::path real(const fs::path &path) {
    std::error_code error;
    const auto result = fs::canonical(path, error);
    return error ? fs::path{} : result;
}
bool below(const fs::path &path, const fs::path &parent) {
    const auto text = path.string(), prefix = parent.string() + "/";
    return !parent.empty() && text.compare(0, prefix.size(), prefix) == 0;
}
bool write(const char *path, const char *text) {
    const auto file = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        std::fprintf(stderr, "Cannot open %s: %s\n", path, std::strerror(errno));
        return false;
    }
    const auto size = std::strlen(text);
    const auto written = ::write(file, text, size);
    const auto error = errno;
    close(file);
    if (written != static_cast<ssize_t>(size)) {
        std::fprintf(stderr, "Cannot write %s: %s\n", path, std::strerror(error));
        return false;
    }
    return true;
}
int command(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    const auto child = fork();
    if (child == 0) {
        execv(argv.front(), argv.data());
        _exit(127);
    }
    if (child < 0) {
        return -1;
    }
    int status{};
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
bool preflight() {
    const auto root = real(device);
    if (root.empty() || read(root / "vendor") != "0x1022" ||
        read(root / "device") != "0x15c0" || read(root / "revision") != "0x00" ||
        read(root / "subsystem_vendor") != "0x1022" ||
        read(root / "subsystem_device") != "0x15c0" ||
        real(root / "driver").filename() != "xhci_hcd" ||
        real(root / "iommu_group").filename() != "37") {
        std::fprintf(stderr, "Controller identity, original driver or IOMMU group changed.\n");
        return false;
    }
    const auto override = read(root / "driver_override");
    if (!override.empty() && override != "(null)") {
        std::fprintf(stderr, "An existing driver override must be preserved by its owner.\n");
        return false;
    }
    std::error_code error;
    unsigned functions{};
    for (fs::directory_iterator it(root / "iommu_group/devices", error), end;
         !error && it != end; it.increment(error)) {
        if (it->path().filename() != bdf) {
            return false;
        }
        ++functions;
    }
    if (error || functions != 1) {
        return false;
    }
    unsigned usb_devices{};
    for (fs::directory_iterator it("/sys/bus/usb/devices", error), end;
         !error && it != end; it.increment(error)) {
        const auto path = it->path();
        const auto name = path.filename().string();
        if (name.find(':') != std::string::npos || name.compare(0, 3, "usb") == 0 ||
            !below(real(path), root)) {
            continue;
        }
        if (read(path / "idVendor") != "0781" || read(path / "idProduct") != "5567" ||
            read(path / "serial") != serial) {
            std::fprintf(stderr, "Another USB device is attached to the selected controller.\n");
            return false;
        }
        ++usb_devices;
    }
    if (error || usb_devices != 1) {
        std::fprintf(stderr, "The exact selected SanDisk must be the sole attached USB device.\n");
        return false;
    }
    std::set<std::string> block_devices, names;
    unsigned disks{};
    for (fs::directory_iterator it("/sys/class/block", error), end;
         !error && it != end; it.increment(error)) {
        const auto path = it->path();
        if (!below(real(path), root)) {
            continue;
        }
        block_devices.insert(read(path / "dev"));
        names.insert("/dev/" + path.filename().string());
        if (!fs::exists(path / "partition", error)) {
            ++disks;
            if (read(path / "size") != "240328704" || read(path / "removable") != "1") {
                std::fprintf(stderr, "Selected disk size/removability changed.\n");
                return false;
            }
        }
    }
    if (error || disks != 1 || block_devices.size() != 3) {
        std::fprintf(stderr, "The expected SanDisk disk and two partitions must be present.\n");
        return false;
    }
    std::ifstream mounts("/proc/self/mountinfo"), swaps("/proc/swaps");
    if (!mounts || !swaps) {
        return false;
    }
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string id, parent, number;
        if (!(fields >> id >> parent >> number) || block_devices.count(number)) {
            std::fprintf(stderr, "SanDisk is mounted; no handoff attempted.\n");
            return false;
        }
    }
    while (std::getline(swaps, line)) {
        std::istringstream fields(line);
        std::string name;
        fields >> name;
        if (names.count(name)) {
            return false;
        }
    }
    if (fs::exists(group_node, error) || error) {
        std::fprintf(stderr, "A VFIO group node already exists; another owner may be using it.\n");
        return false;
    }
    std::puts("Preflight passed: exact AMD controller/group, sole SanDisk, no mounts/swap.");
    return true;
}
struct Session {
    bool override_set{}, detached{}, acl_set{};
    pid_t child{-1};
    bool restore() {
        if (child > 0) {
            kill(child, SIGTERM);
            for (unsigned i = 0; i < 100; ++i) {
                int status{};
                const auto result = waitpid(child, &status, WNOHANG);
                if (result == child || (result < 0 && errno == ECHILD)) {
                    child = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (child > 0) {
                kill(child, SIGKILL);
                int status{};
                if (waitpid(child, &status, 0) != child) {
                    std::fprintf(stderr, "Cannot join VM; controller remains quarantined in VFIO.\n");
                    return false;
                }
                child = -1;
            }
        }
        if (acl_set) {
            if (command({"/usr/bin/setfacl", "-x", "u:1000", group_node}) != 0) {
                return false;
            }
            acl_set = false;
        }
        const auto current = real(fs::path(device) / "driver").filename().string();
        if (detached && current == "vfio-pci" &&
            !write("/sys/bus/pci/drivers/vfio-pci/unbind", bdf)) {
            return false;
        }
        if (override_set && !write("/sys/bus/pci/devices/0000:13:00.3/driver_override", "\n")) {
            return false;
        }
        override_set = false;
        if (detached && real(fs::path(device) / "driver").filename() != "xhci_hcd" &&
            !write("/sys/bus/pci/drivers/xhci_hcd/bind", bdf)) {
            return false;
        }
        detached = false;
        const bool ok = real(fs::path(device) / "driver").filename() == "xhci_hcd";
        std::puts(ok ? "Original xhci_hcd binding and empty override restored."
                     : "Restoration incomplete; local recovery is required.");
        return ok;
    }
    ~Session() {
        if (override_set || detached || acl_set || child > 0) {
            restore();
        }
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--check") == 0) {
        return preflight() ? 0 : 2;
    }
    if (argc != 4 || std::strcmp(argv[1], "--run") || geteuid() != 0) {
        std::fprintf(stderr, "usage: physical-session --check | pkexec physical-session --run QEMU_ARGS SHA256\n");
        return 2;
    }
    struct stat information{};
    if (lstat(argv[2], &information) || !S_ISREG(information.st_mode) ||
        information.st_uid != caller_uid || information.st_size < 1 ||
        information.st_size > 65536) {
        return 2;
    }
    const auto file = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    std::string content(static_cast<std::size_t>(information.st_size), '\0');
    const auto received = file < 0 ? -1 : ::read(file, content.data(), content.size());
    if (file >= 0) {
        close(file);
    }
    if (received != static_cast<ssize_t>(content.size())) {
        return 2;
    }
    lightning::packages::Sha256 hash;
    hash.update(reinterpret_cast<const unsigned char *>(content.data()), content.size());
    if (hash.finish() != argv[3]) {
        std::fprintf(stderr, "Reviewed VM argument identity changed; no handoff attempted.\n");
        return 2;
    }
    std::istringstream input(content);
    std::vector<std::string> args{"/usr/bin/qemu-system-i386"};
    std::string line;
    unsigned vfio{};
    while (std::getline(input, line)) {
        if (line.empty() || line.size() > 8192 || line.find('\0') != std::string::npos ||
            args.size() >= 96) {
            return 2;
        }
        if (line.find("vfio-pci") != std::string::npos) {
            if (line != "vfio-pci,host=0000:13:00.3,id=physical-xhci") {
                return 2;
            }
            ++vfio;
        }
        args.push_back(line);
    }
    if (!input.eof() || vfio != 1 || !preflight()) {
        return 2;
    }
    // VFIO pins the fixed 1 GiB guest RAM after dropping privileges. Set only
    // this session's inherited allowance, with room for the bounded DMA maps.
    constexpr rlim_t pinned_memory_limit = 1536ULL * 1024ULL * 1024ULL;
    const rlimit pinned_memory{pinned_memory_limit, pinned_memory_limit};
    if (setrlimit(RLIMIT_MEMLOCK, &pinned_memory) != 0) {
        std::fprintf(stderr, "Cannot set bounded VFIO memory allowance: %s\n",
                     std::strerror(errno));
        return 1;
    }
    std::vector<char *> command_line;
    for (auto &arg : args) {
        command_line.push_back(arg.data());
    }
    command_line.push_back(nullptr);
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
    std::signal(SIGHUP, signal_stop);
    Session session;
    if (command({"/usr/sbin/modprobe", "vfio-pci"}) != 0 ||
        !write("/sys/bus/pci/devices/0000:13:00.3/driver_override", "vfio-pci\n")) {
        return 1;
    }
    session.override_set = true;
    if (!write("/sys/bus/pci/drivers/xhci_hcd/unbind", bdf)) {
        return 1;
    }
    session.detached = true;
    if (!write("/sys/bus/pci/drivers_probe", bdf) ||
        real(fs::path(device) / "driver").filename() != "vfio-pci") {
        return 1;
    }
    for (unsigned i = 0; i < 50 && access(group_node, F_OK) != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (command({"/usr/bin/setfacl", "-m", "u:1000:rw", group_node}) != 0) {
        return 1;
    }
    session.acl_set = true;
    session.child = fork();
    if (session.child == 0) {
        if (setgroups(0, nullptr) || setgid(caller_gid) || setuid(caller_uid) ||
            prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
            _exit(126);
        }
        execv(command_line.front(), command_line.data());
        _exit(127);
    }
    if (session.child < 0) {
        return 1;
    }
    std::printf("Owned test VM PID=%ld, controller=%s; maximum session20minutes.\n",
                static_cast<long>(session.child), bdf);
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    int status{};
    bool exited{};
    while (!interrupted && std::chrono::steady_clock::now() < deadline) {
        const auto result = waitpid(session.child, &status, WNOHANG);
        if (result == session.child) {
            session.child = -1;
            exited = true;
            break;
        }
        if (result < 0 && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool restored = session.restore();
    return exited && restored && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

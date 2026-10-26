// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tools.inventory
Purpose: Capture bounded Linux sysfs identity data without opening PCI config or MMIO.
Created: 2026-10-25
Architecture: Host user-mode collector; board matching is shared with native code.
*/
#include "../core/controller.hpp"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace {
namespace fs = std::filesystem;
constexpr std::size_t maximum_value_bytes = 256;
constexpr std::size_t maximum_pci_functions = 256 * 32 * 8;
bool read_value(const fs::path &path, std::string &value) {
    value.clear();
    std::ifstream file(path);
    char c{};
    while (file.get(c)) {
        if (c == '\n' || c == '\r') {
            break;
        }
        if (value.size() >= maximum_value_bytes || static_cast<unsigned char>(c) < 32) {
            return false;
        }
        value += c;
    }
    return !value.empty();
}
bool number(const fs::path &path, std::uint32_t maximum, std::uint32_t &value) {
    std::string text;
    if (!read_value(path, text)) {
        return false;
    }
    const char *first = text.data();
    if (text.size() >= 2 && text.substr(0, 2) == "0x") {
        first += 2;
    }
    const auto result = std::from_chars(first, text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= maximum;
}
std::string quote(const std::string &value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        if (c < 32 || c > 126) {
            result += '?';
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}
} // namespace
int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "Usage: xhci_inventory [sysfs-root]\n";
        return 1;
    }
    const fs::path sys = argc == 2 ? argv[1] : "/sys";
    const auto root = sys / "bus/pci/devices";
    std::error_code error;
    std::vector<fs::path> paths;
    for (fs::directory_iterator i(root, error), end; !error && i != end; i.increment(error)) {
        if (paths.size() >= maximum_pci_functions) {
            std::cerr << "PCI sysfs inventory exceeds its bounded limit\n";
            return 1;
        }
        paths.push_back(i->path());
    }
    if (error) {
        std::cerr << "Cannot enumerate PCI sysfs\n";
        return 1;
    }
    std::sort(paths.begin(), paths.end());
    std::string board, vendor, version;
    if (!read_value(sys / "class/dmi/id/board_name", board) ||
        !read_value(sys / "class/dmi/id/board_vendor", vendor) ||
        !read_value(sys / "class/dmi/id/board_version", version)) {
        std::cerr << "Cannot read board identity\n";
        return 1;
    }
    std::cout << "{\n  \"schema\":1,\n  \"scope\":\"read-only sysfs; no MMIO/config access or "
                 "rebinding\",\n"
              << "  \"board\":{\"name\":" << quote(board) << ",\"vendor\":" << quote(vendor)
              << ",\"version\":" << quote(version) << "},\n  \"controllers\":[\n";
    unsigned found{}, admitted{};
    bool malformed{};
    for (const auto &path : paths) {
        std::uint32_t pci_class{};
        if (!number(path / "class", 0xffffff, pci_class) || pci_class != xhci::pci_xhci_class) {
            continue;
        }
        std::uint32_t ven{}, dev{}, sven{}, sdev{}, rev{};
        if (!number(path / "vendor", 0xffff, ven) || !number(path / "device", 0xffff, dev) ||
            !number(path / "subsystem_vendor", 0xffff, sven) ||
            !number(path / "subsystem_device", 0xffff, sdev) ||
            !number(path / "revision", 0xff, rev)) {
            malformed = true;
            continue;
        }
        const xhci::PciIdentity identity{
            static_cast<std::uint16_t>(ven), static_cast<std::uint16_t>(dev),
            static_cast<std::uint16_t>(sven), static_cast<std::uint16_t>(sdev),
            static_cast<std::uint8_t>(rev)};
        const auto *profile = xhci::match_board_controller(identity);
        if (profile) {
            ++admitted;
        }
        const auto driver = fs::read_symlink(path / "driver", error);
        const std::string driver_name = error ? "unavailable" : driver.filename().string();
        error.clear();
        const auto group = fs::read_symlink(path / "iommu_group", error);
        const std::string group_name = error ? "unavailable" : group.filename().string();
        error.clear();
        if (found++) {
            std::cout << ",\n";
        }
        std::cout << "    {\"bdf\":" << quote(path.filename().string()) << ",\"vendor\":" << ven
                  << ",\"device\":" << dev << ",\"subsystem_vendor\":" << sven
                  << ",\"subsystem_device\":" << sdev << ",\"revision\":" << rev
                  << ",\"linux_driver\":" << quote(driver_name)
                  << ",\"iommu_group\":" << quote(group_name)
                  << ",\"profile\":" << quote(profile ? profile->name : "unreviewed")
                  << ",\"identity_matches_profile\":" << (profile ? "true" : "false") << '}';
    }
    std::cout << "\n  ],\n  \"identity_matches\":" << admitted
              << ",\n  \"physical_controller_tested\":false\n}\n";
    return !malformed && found && admitted ? 0 : 2;
}

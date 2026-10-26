// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.firmware
Purpose: Exercise BIOS refusal, delayed ownership and precise legacy SMI write values.
Created: 2026-10-25
Architecture: Synthetic register provider only; never accesses host controller hardware.
*/
#include "firmware.hpp"
#include "resources.hpp"
#include <cstdio>
namespace {
struct Fake {
    std::uint32_t header{1u | (1u << 16)}, control{0xffffffffu & ~(1u << 28)};
    unsigned waits{}, release_after{3}, writes{};
    bool unreadable{}, fail_write{}, fail_wait{}, sticky_smi{};
};
bool read(void *context, std::uint32_t offset, std::uint32_t &value) noexcept {
    auto &fake = *static_cast<Fake *>(context);
    if (fake.unreadable || (offset != 0x40 && offset != 0x44)) {
        return false;
    }
    value = offset == 0x40 ? fake.header : fake.control;
    return true;
}
bool write(void *context, std::uint32_t offset, std::uint32_t value) noexcept {
    auto &fake = *static_cast<Fake *>(context);
    ++fake.writes;
    if (fake.fail_write) {
        return false;
    }
    if (offset == 0x40) {
        fake.header = value;
    } else if (offset == 0x44) {
        fake.control = value & ~(7u << 29);
        if (fake.sticky_smi) {
            fake.control |= 1;
        }
    } else {
        return false;
    }
    return true;
}
bool wait(void *context, std::uint32_t ms) noexcept {
    auto &fake = *static_cast<Fake *>(context);
    if (ms != 1 || fake.fail_wait) {
        return false;
    }
    if (++fake.waits == fake.release_after) {
        fake.header &= ~(1u << 16);
    }
    return true;
}
unsigned checks{}, failures{};
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
xhci::HandoffResult run(Fake &fake, xhci::FirmwareSnapshot &snapshot, unsigned offset = 0x40) {
    xhci::ProtocolMap protocols{};
    protocols.legacy_support_offset = offset;
    return xhci::acquire_firmware({{&fake, 0x100, read}, write, wait}, protocols, snapshot);
}
} // namespace
int main() {
    using xhci::HandoffResult;
    Fake fake{};
    xhci::FirmwareSnapshot snapshot{};
    require(run(fake, snapshot) == HandoffResult::complete, "delayed BIOS grants ownership");
    require(snapshot.waited_milliseconds == 3 && snapshot.os_owned && fake.writes == 2,
            "bounded wait and two writes");
    require(snapshot.initial_legacy == (1u | (1u << 16)), "initial snapshot retained");
    require(snapshot.final_control == ((7u << 1) | (255u << 5) | (7u << 17)),
            "reserved preserved; SMI disabled and W1C acknowledged");
    fake = {};
    fake.release_after = 1001;
    require(run(fake, snapshot) == HandoffResult::timeout, "BIOS refusal times out");
    require(fake.waits == 1000 && fake.writes == 1 && (fake.header & (1u << 16)),
            "never forces BIOS ownership or writes control after refusal");
    fake = {};
    require(run(fake, snapshot, 0) == HandoffResult::complete && fake.writes == 0 &&
                !snapshot.present,
            "absent capability has no writes");
    require(run(fake, snapshot, 0xfc) == HandoffResult::invalid && fake.writes == 0,
            "truncated capability refused");
    fake = {};
    fake.unreadable = true;
    require(run(fake, snapshot) == HandoffResult::inaccessible && fake.writes == 0,
            "inaccessible mapping refused");
    fake = {};
    fake.fail_write = true;
    require(run(fake, snapshot) == HandoffResult::inaccessible, "write failure propagated");
    fake = {};
    fake.fail_wait = true;
    require(run(fake, snapshot) == HandoffResult::inaccessible, "sleep failure propagated");
    fake = {};
    fake.sticky_smi = true;
    require(run(fake, snapshot) == HandoffResult::smi_enabled && !snapshot.os_owned,
            "SMI reenable refuses activation");
    fake = {};
    fake.header = 1;
    require(run(fake, snapshot) == HandoffResult::complete && fake.waits == 0,
            "unowned firmware no sleep");
    using xhci::valid_register_assignment;
    require(valid_register_assignment(0x1000, 0x1000, 0x90001000, 0x10000, 0x10000, false),
            "OS-translated BAR admitted");
    require(valid_register_assignment(0x100001000ull, 0x100001000ull, 0x200001000ull, 0x10000,
                                      0x10000, false),
            "64-bit BAR retained");
    require(!valid_register_assignment(0x1000, 0x2000, 0x90001000, 0x10000, 0x10000, false),
            "unrelated BAR refused");
    require(
        !valid_register_assignment(0x1000, 0x1000, 0xfffffffffffffff0ull, 0x10000, 0x10000, false),
        "physical span overflow refused");
    require(!valid_register_assignment(0x1000, 0x1000, 0x90001000, 0x10000, 0x1000, false),
            "translated size mismatch refused");
    require(!valid_register_assignment(0x1000, 0x1000, 0x90001000, 0, 0, false),
            "zero length refused");
    require(!valid_register_assignment(0x1000, 0x1000, 0x90001000, 0x10000, 0x10000, true),
            "read-only BAR refused");
    require(!valid_register_assignment(0x1000, 0x1000, 0x90001001, 0x10000, 0x10000, false),
            "unaligned mapping refused");
    require(!valid_register_assignment(0x1000, 0x1000, 0x90001000, 0x4000001, 0x4000001, false),
            "mapping ceiling enforced");
    std::printf("firmware checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

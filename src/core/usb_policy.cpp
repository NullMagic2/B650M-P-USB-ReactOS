// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.usb_policy
Purpose: Bounded USB2/SuperSpeed endpoint and hub context policies.
Created: 2026-10-25
Architecture: Pure descriptor decoding called by the native configuration owner.
*/
#include "usb_policy.hpp"
namespace xhci {
namespace {
std::uint16_t little16(const std::uint8_t *bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<unsigned>(bytes[1]) << 8));
}
bool valid_speed(UsbSpeed speed) noexcept {
    return speed >= UsbSpeed::full && speed <= UsbSpeed::super_plus;
}
} // namespace
bool interrupt_interval(UsbSpeed speed, std::uint8_t value, std::uint8_t &interval) noexcept {
    interval = 0;
    if (!valid_speed(speed) || !value) {
        return false;
    }
    if (speed == UsbSpeed::high || speed >= UsbSpeed::super) {
        if (value > 16) {
            return false;
        }
        interval = value - 1;
        return true;
    }
    // Round down to a supported power of two: poll no less often than requested.
    unsigned frames = value;
    interval = 3;
    while (frames >= 2 && interval < 10) {
        frames >>= 1;
        ++interval;
    }
    return true;
}
bool endpoint_policy(UsbSpeed speed, const std::uint8_t *descriptor, std::size_t bytes,
                     const std::uint8_t *companion, std::size_t companion_bytes,
                     EndpointPolicy &policy) noexcept {
    policy = EndpointPolicy{};
    if (!valid_speed(speed) || !descriptor || bytes < 7 || descriptor[0] < 7 ||
        descriptor[0] > bytes || descriptor[1] != 5 || (descriptor[2] & 0x70)) {
        return false;
    }
    const unsigned number = descriptor[2] & 15;
    const unsigned transfer = descriptor[3] & 3;
    const bool input = (descriptor[2] & 0x80) != 0;
    const unsigned wire_packet = little16(descriptor + 4);
    const unsigned packet = wire_packet & 0x7ff;
    const unsigned transactions = (wire_packet >> 11) & 3;
    if (!packet || (wire_packet & 0xe000) || transfer == 1 || ((number == 0) != (transfer == 0)) ||
        (descriptor[3] & 0xfc)) {
        return false;
    }
    EndpointPolicy candidate{};
    candidate.max_packet = static_cast<std::uint16_t>(packet);
    candidate.dci = static_cast<std::uint8_t>(number ? 2 * number + input : 1);
    candidate.type = static_cast<std::uint8_t>(transfer == 0   ? 4
                                               : transfer == 2 ? (input ? 6 : 2)
                                                               : (input ? 7 : 3));
    if (transfer == 0) {
        if (transactions || companion || (speed == UsbSpeed::low && packet != 8) ||
            (speed == UsbSpeed::full && packet != 8 && packet != 16 && packet != 32 &&
             packet != 64) ||
            (speed == UsbSpeed::high && packet != 64) ||
            (speed >= UsbSpeed::super && packet != 512)) {
            return false;
        }
    } else if (transfer == 2) {
        if (speed == UsbSpeed::low || transactions ||
            (speed == UsbSpeed::full && packet != 8 && packet != 16 && packet != 32 &&
             packet != 64) ||
            (speed == UsbSpeed::high && packet != 512) ||
            (speed >= UsbSpeed::super && packet != 1024)) {
            return false;
        }
    } else {
        if (!interrupt_interval(speed, descriptor[6], candidate.interval) ||
            (speed == UsbSpeed::low && packet > 8) || (speed == UsbSpeed::full && packet > 64) ||
            packet > 1024 || transactions == 3 || (transactions && speed != UsbSpeed::high)) {
            return false;
        }
        candidate.burst = static_cast<std::uint8_t>(transactions);
        candidate.bytes_per_interval = packet * (transactions + 1);
    }
    if (speed >= UsbSpeed::super && transfer != 0) {
        if (!companion || companion_bytes < 6 || companion[0] < 6 ||
            companion[0] > companion_bytes || companion[1] != 0x30 || companion[2] > 15 ||
            companion[3] != 0) {
            return false;
        }
        candidate.burst = companion[2];
        const unsigned interval_bytes = little16(companion + 4);
        if ((transfer == 2 && interval_bytes) ||
            (transfer == 3 &&
             (!interval_bytes || interval_bytes > packet * (candidate.burst + 1)))) {
            return false;
        }
        candidate.bytes_per_interval = interval_bytes;
    } else if (companion) {
        return false;
    }
    candidate.context0 = (static_cast<std::uint32_t>(candidate.interval) << 16) |
                         ((candidate.bytes_per_interval >> 16) << 24);
    candidate.context1 = 6u | (static_cast<std::uint32_t>(candidate.type) << 3) |
                         (static_cast<std::uint32_t>(candidate.burst) << 8) | (packet << 16);
    candidate.context4 = (candidate.bytes_per_interval << 16) |
                         static_cast<std::uint32_t>(transfer == 0 ? 8 : packet);
    policy = candidate;
    return true;
}
bool child_route(const UsbRoute *parent, UsbSpeed parent_speed, std::uint8_t parent_slot,
                 std::uint8_t port, UsbSpeed child_speed, UsbRoute &route,
                 bool parent_multi_tt) noexcept {
    route = UsbRoute{};
    if (!port || !valid_speed(child_speed)) {
        return false;
    }
    if (!parent) {
        route.root_port = port;
        return true;
    }
    if (!parent->root_port || parent->depth >= 5 || !parent_slot || !valid_speed(parent_speed) ||
        (parent->string >> (parent->depth * 4)) ||
        (parent_speed >= UsbSpeed::super) != (child_speed >= UsbSpeed::super) ||
        parent_speed == UsbSpeed::low ||
        (parent_speed == UsbSpeed::full && child_speed == UsbSpeed::high)) {
        return false;
    }
    route = *parent;
    route.string |= static_cast<std::uint32_t>(port > 15 ? 15 : port) << (parent->depth * 4);
    route.depth = parent->depth + 1;
    route.parent_slot = parent_slot;
    route.parent_port = port;
    if ((child_speed == UsbSpeed::low || child_speed == UsbSpeed::full) &&
        parent_speed == UsbSpeed::high) {
        route.tt_slot = parent_slot;
        route.tt_port = port;
        route.tt_multi = parent_multi_tt;
    }
    return true;
}
bool hub_policy(UsbSpeed speed, const std::uint8_t *descriptor, std::size_t bytes,
                std::uint8_t protocol, HubPolicy &policy) noexcept {
    policy = HubPolicy{};
    if (!valid_speed(speed) || speed == UsbSpeed::low || !descriptor || bytes < 7 ||
        descriptor[0] > bytes || descriptor[2] == 0) {
        return false;
    }
    const bool super_speed = speed >= UsbSpeed::super;
    const unsigned bitmap_bytes = (static_cast<unsigned>(descriptor[2]) + 8) / 8;
    if ((super_speed &&
         (descriptor[0] < 12 || descriptor[1] != 0x2a || descriptor[2] > 15 || protocol != 3)) ||
        (!super_speed && (descriptor[0] < 7 + 2 * bitmap_bytes || descriptor[1] != 0x29 ||
                          protocol > 2 || (speed == UsbSpeed::full && protocol != 0)))) {
        return false;
    }
    const unsigned characteristics = little16(descriptor + 3);
    if (characteristics & 0xff00) {
        return false;
    }
    policy.ports = descriptor[2];
    policy.characteristics = static_cast<std::uint16_t>(characteristics);
    policy.power_good_ms = static_cast<std::uint16_t>(descriptor[5] * 2);
    policy.super_speed = super_speed;
    policy.multi_tt = !super_speed && protocol == 2;
    policy.tt_think_time =
        speed == UsbSpeed::high ? static_cast<std::uint8_t>((characteristics >> 5) & 3) : 0;
    return true;
}
void port_status(bool super_speed, std::uint32_t portsc, std::uint16_t &status,
                 std::uint16_t &change) noexcept {
    status = static_cast<std::uint16_t>((portsc & 1) | ((portsc & 2)) | ((portsc & 8)) |
                                        ((portsc & 16)));
    if (super_speed) {
        status |= static_cast<std::uint16_t>(portsc & 0x1e0);
        status |= static_cast<std::uint16_t>((portsc & (1u << 9)) ? 1u << 9 : 0);
        // USB3 wPortStatus speed field is zero for SuperSpeed; link-state is native.
    } else {
        status |= static_cast<std::uint16_t>((portsc & (1u << 9)) ? 1u << 8 : 0);
        const unsigned speed = (portsc >> 10) & 15;
        status |= static_cast<std::uint16_t>(speed == 2 ? 1u << 9 : speed == 3 ? 1u << 10 : 0);
        if (((portsc >> 5) & 15) == 3) {
            status |= 4;
        }
    }
    change = static_cast<std::uint16_t>(((portsc >> 17) & 1) | (((portsc >> 18) & 1) << 1) |
                                        (((portsc >> 20) & 1) << 3) | (((portsc >> 21) & 1) << 4));
    if (super_speed) {
        change &= static_cast<std::uint16_t>(~2u); // USB3 reserves the USB2 enable-change bit.
        change |=
            static_cast<std::uint16_t>((((portsc >> 19) & 1) << 5) | (((portsc >> 22) & 1) << 6) |
                                       (((portsc >> 23) & 1) << 7));
    } else if (portsc & (1u << 22)) {
        change |= 4;
    }
}
} // namespace xhci

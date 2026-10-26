// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.usb_policy
Purpose: Validate USB endpoint, hub route and protocol-specific port wire contracts.
Created: 2026-10-25
Architecture: Native enumeration and transfer owners use these allocation-free encodings.
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include "controller.hpp"
namespace xhci {
// Values match xHCI default Protocol Speed IDs, not the legacy USB_DEVICE_SPEED enum.
enum class UsbSpeed : std::uint8_t { full = 1, low = 2, high = 3, super = 4, super_plus = 5 };
struct EndpointPolicy {
    std::uint32_t context0{}, context1{}, context4{};
    std::uint32_t bytes_per_interval{};
    std::uint16_t max_packet{};
    std::uint8_t dci{}, type{}, interval{}, burst{}, mult{};
};
// No allocation or ownership transfer. A companion must immediately follow a SuperSpeed
// non-control endpoint. Streams/isochronous endpoints are outside initial scope.
bool endpoint_policy(UsbSpeed speed, const std::uint8_t *endpoint, std::size_t endpoint_bytes,
                     const std::uint8_t *companion, std::size_t companion_bytes,
                     EndpointPolicy &policy) noexcept;
struct UsbRoute {
    std::uint32_t string{};
    std::uint8_t root_port{}, depth{}, tt_slot{}, tt_port{}, parent_slot{}, parent_port{};
    bool tt_multi{};
};
// parent is null only for a root-connected device. Root ports are controller physical
// port IDs; downstream ports are one-based hub IDs. Parent lifetime is caller-owned.
bool child_route(const UsbRoute *parent, UsbSpeed parent_speed, std::uint8_t parent_slot,
                 std::uint8_t port, UsbSpeed child_speed, UsbRoute &route,
                 bool parent_multi_tt = false) noexcept;
struct HubPolicy {
    std::uint8_t ports{}, tt_think_time{};
    bool multi_tt{}, super_speed{};
    std::uint16_t characteristics{};
    std::uint16_t power_good_ms{};
};
// USB2 type0x29 and SuperSpeed type0x2a descriptors retain their native wire layout.
bool hub_policy(UsbSpeed speed, const std::uint8_t *descriptor, std::size_t bytes,
                std::uint8_t interface_protocol, HubPolicy &policy) noexcept;
// USB2 milliseconds and SuperSpeed/HS exponential intervals have distinct rules.
bool interrupt_interval(UsbSpeed speed, std::uint8_t descriptor_interval,
                        std::uint8_t &context_interval) noexcept;
// Translate protocol-native xHCI PORTSC to USB class status/change; never label SS high speed.
void port_status(bool super_speed, std::uint32_t portsc, std::uint16_t &status,
                 std::uint16_t &change) noexcept;
// Resolve a controller-local Protocol Speed ID using its Supported Protocol PSI
// table. Native slot contexts must retain the original ID rather than this enum.
bool resolve_port_speed(const RegisterReader &, const Capabilities &, std::uint8_t physical_port,
                        std::uint8_t protocol_speed_id, UsbSpeed &) noexcept;
} // namespace xhci

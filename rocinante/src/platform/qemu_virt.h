/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

namespace Rocinante::Platform::QemuVirt {

// QEMU LoongArch virt: VIRT_UART_BASE address
constexpr std::uintptr_t kUartBase = 0x1fe001e0UL;

// QEMU LoongArch virt: syscon-poweroff MMIO base
constexpr std::uintptr_t kSysconBase = 0x100e001cUL;

// QEMU LoongArch64 virt poweroff is wired up as a "syscon-poweroff" device.
// The virt machine advertises this via its DTB:
// - /poweroff compatible = "syscon-poweroff"
// - regmap -> syscon at 0x100e001c (reg-io-width = 1)
// - offset = 0, value = 0x34
// Writing that byte triggers a QEMU shutdown event, and QEMU exits by default
// (-action shutdown=poweroff).
constexpr std::uintptr_t kPoweroffOffset = 0;
constexpr std::uint8_t kPoweroffValue = 0x34;

} // namespace Rocinante::Platform::QemuVirt

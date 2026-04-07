/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/platform/power.h>

#include <src/platform/qemu_virt.h>
#include <src/sp/mmio.h>

namespace {

std::uintptr_t g_syscon_base_address = Rocinante::Platform::QemuVirt::kSysconBase;

} // namespace

namespace Rocinante::Platform {

void SetSysconBaseAddress(std::uintptr_t base_address) {
	if (base_address != 0) g_syscon_base_address = base_address;
}

[[noreturn]] void Halt() {
	for (;;) {
		asm volatile("idle 0" ::: "memory");
	}
}

[[noreturn]] void Shutdown() {
	Rocinante::MMIO<8>::write(
		g_syscon_base_address + Rocinante::Platform::QemuVirt::kPoweroffOffset,
		Rocinante::Platform::QemuVirt::kPoweroffValue
	);
	asm volatile("dbar 0" ::: "memory");

	// If QEMU ignores the poweroff request, just stop.
	Halt();
}

} // namespace Rocinante::Platform

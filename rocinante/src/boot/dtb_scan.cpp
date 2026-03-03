/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/boot/dtb_scan.h>

#include <src/memory/boot_memory_map.h>

#include <cstddef>
#include <cstdint>

namespace Rocinante::Boot {

const void* TryLocateDeviceTreeBlobPointerFromBootInfoRegion() {
	// QEMU's direct-kernel boot commonly places the DTB in low physical memory.
	// Our linker script intentionally keeps the kernel image clear of this area.
	//
	// We do not yet parse the EFI system table to locate FDT/ACPI tables.
	// For current bring-up (especially QEMU direct-kernel boot), we use this
	// scan as a heuristic to locate a valid FDT header in the conventional
	// low-memory "boot info" area.
	// Search range policy:
	// - Start at 0x4 instead of 0x0 so we never pass a null pointer.
	// - Search the first 16 MiB, which is a common area for firmware/boot blobs.
	static constexpr std::uintptr_t kSearchBeginPhysical = 0x00000004UL;
	static constexpr std::uintptr_t kSearchEndPhysical = 0x01000000UL;
	static constexpr std::size_t kSearchStepBytes = 4;

	for (std::uintptr_t candidate = kSearchBeginPhysical; (candidate + 4) < kSearchEndPhysical; candidate += kSearchStepBytes) {
		const void* p = reinterpret_cast<const void*>(candidate);
		if (!Rocinante::Memory::BootMemoryMap::LooksLikeDeviceTreeBlob(p)) continue;

		const std::size_t total_size_bytes = Rocinante::Memory::BootMemoryMap::DeviceTreeTotalSizeBytesOrZero(p);
		if (total_size_bytes == 0) continue;
		if ((candidate + total_size_bytes) > kSearchEndPhysical) continue;

		return p;
	}

	return nullptr;
}

} // namespace Rocinante::Boot

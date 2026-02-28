/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante::Memory {

/**
 * @brief Physical memory region reported by the boot environment.
 *
 * This is an *input* to the real physical memory manager (PMM).
 *
 * The boot environment may describe memory using:
 * - a device tree blob (DTB / Flattened Device Tree, "FDT")
 * - a UEFI memory map (future)
 *
 * We intentionally keep this representation small and explicit so it can be
 * printed during early bring-up and consumed by the PMM.
 */
struct BootMemoryRegion final {
	enum class Type : std::uint8_t {
		UsableRAM,
		Reserved,
	};

	std::uint64_t physical_base = 0;
	std::uint64_t size_bytes = 0;
	Type type = Type::Reserved;
};

/**
 * @brief A fixed-capacity list of boot memory regions.
 *
 * Why fixed-capacity:
 * - Keeps early boot deterministic.
 * - Avoids heap dependency while we are still *building* the real memory system.
 *
 * Note: "Reserved" regions win over "UsableRAM" when ranges overlap.
 */
struct BootMemoryMap final {
	static constexpr std::size_t kMaxRegions = 64;

	// NOTE: This array is intentionally left uninitialized.
	//
	// Rationale:
	// - In a freestanding kernel, we avoid accidentally pulling in implicit
	//   `memset`/`memcpy` calls just to zero large objects.
	// - The only authoritative bound is `region_count`; callers must only read
	//   entries in [0, region_count).
	BootMemoryRegion regions[kMaxRegions];
	std::size_t region_count = 0;

	// Resets the map to empty.
	void Clear() {
		region_count = 0;
	}

	// Adds a region. Returns false if capacity is exceeded or input is invalid.
	bool AddRegion(BootMemoryRegion region);

	// Quick structural check for a device tree blob.
	//
	// This does not prove that the DTB is semantically correct; it is only meant
	// to help distinguish (DTB pointer) vs (UEFI system table pointer).
	static bool LooksLikeDeviceTreeBlob(const void* device_tree_blob);

	// Returns the DTB's `totalsize` field in bytes, or 0 if the blob does not
	// look like a valid DTB.
	//
	// This is useful for reserving the DTB blob itself in the PMM.
	static std::size_t DeviceTreeTotalSizeBytesOrZero(const void* device_tree_blob);

	// Parses a DTB/FDT memory map into this BootMemoryMap.
	//
	// Extracts:
	// - Usable RAM from the /memory node's "reg" property.
	// - Reserved ranges from:
	//   - the DTB "memreserve" table
	//   - /reserved-memory children "reg" properties
	//
	// Returns true on success.
	//
	// Failure modes (non-exhaustive):
	// - Invalid DTB header / offsets
	// - Unsupported #address-cells/#size-cells combination
	// - Output map capacity exceeded
	bool TryParseFromDeviceTree(const void* device_tree_blob);
};

} // namespace Rocinante::Memory

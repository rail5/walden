/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>
#include <src/memory/paging.h>

namespace Rocinante::Memory {

class KernelVirtualAddressAllocator;
class PhysicalMemoryManager;

namespace KernelMappings {

struct MappedRange final {
	std::uintptr_t virtual_base;
	std::size_t size_bytes;
};

// Minimal kernel mapping helper (bring-up).
//
// This composes:
// - KernelVirtualAddressAllocator for VA range selection.
// - Paging::MapPage4KiB/UnmapPage4KiB for populating page tables.
//
// Non-goals:
// - No allocation metadata/ownership tracking.
// - No address spaces/ASIDs.
// - No support for non-4KiB pages.
Rocinante::Optional<MappedRange> MapPhysicalRange4KiB(
	PhysicalMemoryManager* pmm,
	const Paging::PageTableRoot& root,
	KernelVirtualAddressAllocator* va_allocator,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	Paging::PagePermissions permissions,
	Paging::AddressSpaceBits address_bits
);

bool UnmapAndFree4KiB(
	const Paging::PageTableRoot& root,
	KernelVirtualAddressAllocator* va_allocator,
	std::uintptr_t virtual_base,
	std::size_t size_bytes,
	Paging::AddressSpaceBits address_bits
);

} // namespace KernelMappings

} // namespace Rocinante::Memory

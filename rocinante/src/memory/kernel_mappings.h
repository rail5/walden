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

// Maps a new virtual range by allocating physical frames from the PMM.
//
// Why this exists (bring-up reality):
// - Many kernel regions we want early (stack, heap, small scratch buffers) are
//   *not* backed by a pre-existing, physically contiguous span. Today our PMM
//   provides `AllocatePage()`/`FreePage()` as a page-granular allocator and does
//   not promise contiguity across multiple calls.
// - In contrast, `MapPhysicalRange4KiB()` is intentionally specialized: it maps
//   an *already-selected* contiguous physical address interval
//   `[physical_base, physical_base + size_bytes)` to a newly allocated VA range.
//   That is a good fit for:
//   - mapping a known contiguous region (if we ever get such a span),
//   - mapping device windows that are naturally contiguous in PA,
//   - mapping a pre-reserved physical pool.
//
// Distinction from MapPhysicalRange4KiB():
// - `MapPhysicalRange4KiB()` assumes the caller owns/chooses the physical span
//   up-front and that it is contiguous; it does not allocate frames.
// - `MapNewRange4KiB()` chooses *virtual* placement via the VA allocator, then
//   allocates one PMM frame per page and maps them.
//
// Failure/rollback semantics (important for correctness):
// - If any PMM allocation or page-table mapping fails partway through, this
//   function rolls back:
//   - unmaps any pages already mapped,
//   - frees the PMM frames it allocated for those pages,
//   - frees the VA range back to the VA allocator.
//
// Ownership limitation (bring-up):
// - On success, the physical frames become part of the new mapping, but this
//   helper does not return a frame list and `UnmapAndFree4KiB()` does not return
//   those frames to the PMM.
// - This is deliberate for bring-up use cases like the higher-half heap where
//   the pages are intended to remain allocated for the life of the kernel.
//   A future VMM should add explicit allocation records so mappings can be
//   torn down and frames reclaimed.
Rocinante::Optional<MappedRange> MapNewRange4KiB(
	PhysicalMemoryManager* pmm,
	const Paging::PageTableRoot& root,
	KernelVirtualAddressAllocator* va_allocator,
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

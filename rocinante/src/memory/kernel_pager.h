/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante::Memory::KernelPager {

// Minimal bring-up kernel pager.
//
// This module intentionally implements only a single policy:
// - Demand-map (allocate + map) 4 KiB pages for faults within a single
//   reserved-but-unmapped kernel virtual range.
//
// Non-goals (bring-up flaws):
// - No metadata/ownership tracking.
// - No user faults / address spaces / ASIDs.
// - No per-page reclamation / unmap policy.
// - Not SMP-safe.

struct LazyMappingRegion final {
	std::uintptr_t virtual_base = 0;
	std::size_t size_bytes = 0;
};

// Configures the lazy-mapped virtual range serviced by the kernel pager.
//
// Ordering:
// - Call this after paging state exists and after the region has been reserved
//   in the kernel VA allocator.
// - The region must be page-aligned and a multiple of 4 KiB.
void ConfigureLazyMappingRegion(LazyMappingRegion region);

// Installs the paging-fault observer hook.
//
// Notes:
// - Passing nullptr clears the hook. This function always installs the kernel
//   pager observer (it does not stack observers).
void Install();

} // namespace Rocinante::Memory::KernelPager

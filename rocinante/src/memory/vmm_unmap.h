/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <src/memory/paging.h>
#include <src/memory/pmm.h>
#include <src/memory/vma.h>

namespace Rocinante::Memory::VmmUnmap {

// Unmaps the entire VMA range (4 KiB granularity).
//
// Semantics (bring-up):
// - For each page in the VMA range:
//   - If mapped, remove the leaf PTE (updates map_count via Paging).
//   - If the VMA owns frames and has an anonymous object, drop object ownership
//     for the page offset.
//
// Notes:
// - This helper intentionally does not define a TLB invalidation policy.
//   Note: page-table updates require INVTLB for immediate hardware
//   enforcement, but the correct scope (per-page/per-ASID/flush-all) is a
//   separate iteration.
bool UnmapVma4KiB(
	PhysicalMemoryManager* pmm,
	const Paging::PageTableRoot& root,
	const VirtualMemoryArea& vma
);

} // namespace Rocinante::Memory::VmmUnmap

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <src/memory/vma.h>
#include <src/trap/trap.h>

namespace Rocinante::Memory::VmmPager {

// Configures the kernel VMA set consulted by the paging-fault observer.
//
// Notes / flaws:
// - This is global mutable state (bring-up).
// - This only affects behavior if the VMM pager observer is installed.
void ConfigureKernelVirtualMemoryAreas(const VirtualMemoryAreaSet* areas);

// Paging-fault observer that wires faults through VMAs and anonymous VM objects.
//
// Policy (bring-up):
// - Only handles kernel-mode (PLV0) faults.
// - Only handles page-invalid load/store (PIL/PIS) faults.
// - Uses per-page TLB invalidation for the active ASID after successful mapping.
// - If the mapping is global (G=1), it also invalidates global TLB entries.
Rocinante::Trap::PagingFaultResult PagingFaultObserver(
	Rocinante::TrapFrame& tf,
	const Rocinante::Trap::PagingFaultEvent& event
);

} // namespace Rocinante::Memory::VmmPager

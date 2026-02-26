/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante::Memory {

// CPU address-space limits derived from CPUCFG.
//
// VALEN / PALEN come from CPUCFG word 0x1 (VALEN, PALEN fields).
// The spec describes accessible address ranges as:
//   Virtual:  [0, 2^VALEN - 1]
//   Physical: [0, 2^PALEN - 1]
//
// Note: The precise privileged-mode rules (direct-map windows, paging modes,
// canonicalization, etc.) are described elsewhere in the manual. This struct is
// meant as a simple "what width did the CPU claim?" snapshot.
struct AddressLimits final {
	std::uint32_t VALEN;
	std::uint32_t PALEN;
	std::uint64_t VirtualMax;  // (2^VALEN - 1) when VALEN < 64
	std::uint64_t PhysicalMax; // (2^PALEN - 1) when PALEN < 64
};

// Early memory initialization.
//
// What it does today:
// - Initializes a bootstrap heap (Heap::InitDefault).
// - Reads CPUCFG once and snapshots VALEN/PALEN.
// - Computes a "recommended" heap virtual start based on the kernel end.
//
// What it does *not* do yet:
// - Build a physical memory map / PMM.
// - Create page tables or map additional heap pages.
//
// In other words: this is the bridge between "we have no allocator" and
// "eventually we will have a real VM-backed heap".
void InitEarly();

// Returns the CPU-reported address limits (valid after InitEarly()).
const AddressLimits& Limits();

// Returns the heap virtual address we *recommend* using once paging is enabled.
//
// This is a policy choice: we "recommend" placing the heap immediately after the
// kernel image in virtual memory. Whether we can actually use that address
// depends on the paging/MMU setup.
std::uintptr_t RecommendedHeapVirtualBase();

// Initializes (or re-initializes) the heap to use a specific virtual region.
//
// Call this after we have:
// 1) a PMM that can provide physical pages
// 2) a VMM/page tables that map those pages into [heap_base, heap_base+size)
void InitHeapAfterPaging(void* heap_base, std::size_t heap_size_bytes);

} // namespace Rocinante::Memory

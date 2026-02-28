/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>

namespace Rocinante::Memory {

class PhysicalMemoryManager;

/**
 * @brief Minimal page table implementation for early kernel bring-up.
 *
 * Scope (what this module is for):
 * - Allocate and populate page tables in memory.
 * - Provide software page table walking for self-checks and unit tests.
 *
 * Explicit non-goals (for now):
 * - We do not yet program LoongArch paging-related CSRs.
 * - We do not yet guarantee that our intermediate-level page table entry
 *   encoding matches the LoongArch hardware page-table walker.
 *
 * Why the limitation exists:
 * - LoongArch paging is CSR- and format-sensitive. We want a clean, testable
 *   page table builder first, and we will only enable paging once we have an
 *   authoritative, end-to-end verified CSR + entry-format implementation.
 */
namespace Paging {

// 4 KiB is the baseline page size for our early paging work.
static constexpr std::size_t kPageSizeBytes = 4096;
static constexpr std::size_t kPageShiftBits = 12;

// With 4 KiB pages, each page table has 512 entries (9-bit index).
static constexpr std::size_t kEntriesPerTable = 512;
static constexpr std::size_t kIndexBitsPerLevel = 9;

static constexpr std::uint64_t kPageOffsetMask = (1ull << kPageShiftBits) - 1ull;
static constexpr std::uint64_t kPageBaseMask = ~kPageOffsetMask;

/**
 * @brief LoongArch PTE bit definitions we currently use.
 *
 * Source of truth:
 * - LoongArch Privileged Architecture spec (page table entry format)
 *
 * Cross-check:
 * - Linux LoongArch pgtable bit positions match the spec for the fields we use.
 *
 * NOTE: this file intentionally keeps the set small. We only define the bits
 * we actively use in early bring-up.
 */
namespace PteBits {
	// LoongArch page-table entry bit positions (LA64).
	//
	// Source of truth:
	// - LoongArch-Vol1-EN.html, Section 5.4.5
	//   Figure 8: "Table entry format for common pages" (4 KiB)
	//   Figure 9: "Table entry format for huge pages"
	//
	// For early bring-up we only build common (4 KiB) page mappings. We still
	// define the high-bit permission fields because they affect address masking.
	//
	// Used by TLB hardware.
	static constexpr std::uint64_t kValid = (1ull << 0);
	static constexpr std::uint64_t kDirty = (1ull << 1);
	static constexpr std::uint64_t kPrivilegeLevelShift = 2;
	static constexpr std::uint64_t kPrivilegeLevelMask = (3ull << kPrivilegeLevelShift);
	static constexpr std::uint64_t kCacheShift = 4;
	static constexpr std::uint64_t kCacheMask = (3ull << kCacheShift);
	static constexpr std::uint64_t kGlobal = (1ull << 6);
	static constexpr std::uint64_t kPresent = (1ull << 7);
	static constexpr std::uint64_t kWrite = (1ull << 8);

	// 64-bit-only permission bits.
	// Spec naming: NR = non-readable, NX = non-executable, RPLV = restrict PLV check.
	static constexpr std::uint64_t kNoRead = (1ull << 61);
	static constexpr std::uint64_t kNoExecute = (1ull << 62);
	static constexpr std::uint64_t kRestrictPrivilegeLevel = (1ull << 63);
}

enum class CacheMode : std::uint8_t {
	StrongUncached = 0,  // _CACHE_SUC
	CoherentCached = 1,  // _CACHE_CC
	WeakUncached = 2,    // _CACHE_WUC
};

enum class AccessPermissions : std::uint8_t {
	ReadOnly,
	ReadWrite,
};

enum class ExecutePermissions : std::uint8_t {
	Executable,
	NoExecute,
};

struct PagePermissions final {
	AccessPermissions access;
	ExecutePermissions execute;
	CacheMode cache;
	bool global;
};

/**
 * @brief A single page table page (4 KiB) containing 512 64-bit entries.
 */
struct alignas(kPageSizeBytes) PageTablePage final {
	std::uint64_t entries[kEntriesPerTable];
};

/**
 * @brief A kernel page table root.
 *
 * `root_physical_address` must point to a page-sized, page-aligned table.
 */
struct PageTableRoot final {
	std::uintptr_t root_physical_address;
};

/**
 * @brief Runtime-reported address width configuration.
 *
 * These values come from CPUCFG (VALEN/PALEN) and describe the implemented
 * virtual and physical address widths.
 */
struct AddressSpaceBits final {
	std::uint8_t virtual_address_bits;
	std::uint8_t physical_address_bits;
};

/**
 * @brief Maps one 4 KiB page.
 *
 * - Allocates intermediate tables from the PMM as needed.
 * - Assumes page tables are directly addressable at their physical addresses
 *   (true pre-paging; later we will require a physmap).
 */
bool MapPage4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_address,
	std::uintptr_t physical_address,
	PagePermissions permissions
);

/**
 * @brief Maps one 4 KiB page using runtime-reported address widths.
 */
bool MapPage4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_address,
	std::uintptr_t physical_address,
	PagePermissions permissions,
	AddressSpaceBits address_bits
);

/**
 * @brief Unmaps one 4 KiB page.
 *
 * This does not currently free empty intermediate tables.
 */
bool UnmapPage4KiB(
	const PageTableRoot& root,
	std::uintptr_t virtual_address
);

/**
 * @brief Unmaps one 4 KiB page using runtime-reported address widths.
 */
bool UnmapPage4KiB(
	const PageTableRoot& root,
	std::uintptr_t virtual_address,
	AddressSpaceBits address_bits
);

/**
 * @brief Translates a virtual address via software page table walking.
 *
 * Returns the physical address on success, or empty if not mapped.
 */
Rocinante::Optional<std::uintptr_t> Translate(
	const PageTableRoot& root,
	std::uintptr_t virtual_address
);

/**
 * @brief Translates a virtual address using runtime-reported address widths.
 */
Rocinante::Optional<std::uintptr_t> Translate(
	const PageTableRoot& root,
	std::uintptr_t virtual_address,
	AddressSpaceBits address_bits
);

/**
 * @brief Allocates and initializes a new root page table.
 */
Rocinante::Optional<PageTableRoot> AllocateRootPageTable(PhysicalMemoryManager* pmm);

/**
 * @brief Maps a contiguous range using 4 KiB pages.
 *
 * Requirements:
 * - virtual_base, physical_base must be page-aligned.
 * - size_bytes must be a multiple of the page size.
 */
bool MapRange4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_base,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	PagePermissions permissions
);

/**
 * @brief Maps a contiguous range using runtime-reported address widths.
 */
bool MapRange4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_base,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	PagePermissions permissions,
	AddressSpaceBits address_bits
);

} // namespace Paging

} // namespace Rocinante::Memory

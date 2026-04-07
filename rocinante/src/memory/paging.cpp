/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "paging.h"

#include <src/memory/pmm.h>
#include <src/memory/paging_state.h>
#include <src/memory/virtual_layout.h>
#include <src/sp/cpucfg.h>

extern "C" void* memset(void* destination, int byte_value, std::size_t byte_count);

namespace Rocinante::Memory::Paging {

namespace {

bool IsPhysMapLeafMapping(
	std::uintptr_t virtual_page_base,
	std::uintptr_t physical_page_base,
	AddressSpaceBits address_bits
) {
	const std::uintptr_t physmap_base = Rocinante::Memory::VirtualLayout::PhysMapBase(address_bits.virtual_address_bits);
	if (virtual_page_base < physmap_base) return false;
	return (virtual_page_base - physmap_base) == physical_page_base;
}

// LoongArch privileged architecture: CSR.CRMD (Current Mode Information).
//
// Spec anchor:
//   - LoongArch-Vol1-EN.html, Section 5.2 (Virtual Address Space and Address Translation Mode)
//   - CRMD.DA=1, CRMD.PG=0 => direct address translation mode
//   - CRMD.DA=0, CRMD.PG=1 => mapped address translation mode
namespace Csr {
	constexpr std::uint32_t kCurrentModeInformation = 0x0; // CSR.CRMD
} // namespace Csr

namespace CurrentModeInformation {
	constexpr std::uint64_t kDirectAddressingEnable = (1ull << 3u); // CRMD.DA
	constexpr std::uint64_t kPagingEnable = (1ull << 4u);           // CRMD.PG
} // namespace CurrentModeInformation

inline std::uint64_t ReadCurrentModeInformation() {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(Csr::kCurrentModeInformation));
	return value;
}

inline bool IsMappedAddressTranslationMode() {
	const std::uint64_t crmd = ReadCurrentModeInformation();
	const bool direct_addressing = (crmd & CurrentModeInformation::kDirectAddressingEnable) != 0;
	const bool paging = (crmd & CurrentModeInformation::kPagingEnable) != 0;
	return (!direct_addressing) && paging;
}

// Software-walker masking policy:
//
// Our PTE encoding places flags in the high bits (e.g. No-Execute at bit 62).
// When extracting the physical address, we must mask out those flags.
//
constexpr std::uint8_t kMaxSupportedLevelCount = 6; // (64 - 12 + 8) / 9 = 6 for 4 KiB pages.

constexpr std::uint8_t BitIndexFromSingleBitMask(std::uint64_t mask) {
	std::uint8_t index = 0;
	while (((mask >> index) & 0x1ull) == 0) {
		index++;
	}
	return index;
}

constexpr std::uint8_t kLowestHighFlagBit =
	(BitIndexFromSingleBitMask(PteBits::kNoRead) < BitIndexFromSingleBitMask(PteBits::kNoExecute))
		? BitIndexFromSingleBitMask(PteBits::kNoRead)
		: BitIndexFromSingleBitMask(PteBits::kNoExecute);

// Highest PALEN we can encode without colliding with the high-bit flags we use.
// If PALEN == kLowestHighFlagBit, the highest physical address bit is
// (PALEN-1) == (kLowestHighFlagBit-1), which is safe.
constexpr std::uint8_t kMaxEncodablePhysicalAddressBits = kLowestHighFlagBit;

Rocinante::Memory::Paging::AddressSpaceBits AddressSpaceBitsFromCPUCFG() {
	const std::uint32_t virtual_address_bits = Rocinante::GetCPUCFG().VirtualAddressBits();
	const std::uint32_t physical_address_bits = Rocinante::GetCPUCFG().PhysicalAddressBits();
	return Rocinante::Memory::Paging::AddressSpaceBits{
		.virtual_address_bits = static_cast<std::uint8_t>(virtual_address_bits),
		.physical_address_bits = static_cast<std::uint8_t>(physical_address_bits),
	};
}

std::uint64_t MaskFromBits(std::uint8_t bits) {
	if (bits >= 64) return ~0ull;
	if (bits == 0) return 0;
	return (1ull << bits) - 1ull;
}

std::uint8_t LevelCountFromVirtualAddressBits(std::uint8_t virtual_address_bits) {
	const auto offset_bits = static_cast<std::uint32_t>(Rocinante::Memory::Paging::kPageShiftBits);
	const auto index_bits_per_level = static_cast<std::uint32_t>(Rocinante::Memory::Paging::kIndexBitsPerLevel);

	const std::uint32_t indexable_bits = (virtual_address_bits > offset_bits) ? (virtual_address_bits - offset_bits) : 0u;
	std::uint32_t level_count = (indexable_bits + index_bits_per_level - 1u) / index_bits_per_level;
	if (level_count == 0) level_count = 1;
	if (level_count > kMaxSupportedLevelCount) return 0;
	return static_cast<std::uint8_t>(level_count);
}

std::uint64_t PhysicalPageBaseMaskFromBits(std::uint8_t physical_address_bits) {
	if (physical_address_bits > kMaxEncodablePhysicalAddressBits) return 0;
	if (physical_address_bits < Rocinante::Memory::Paging::kPageShiftBits) {
		return 0;
	}
	const std::uint64_t physical_address_mask = MaskFromBits(physical_address_bits);
	return physical_address_mask & Rocinante::Memory::Paging::kPageBaseMask;
}

constexpr std::uint64_t IndexMaskForLevel() {
	return (1ull << kIndexBitsPerLevel) - 1ull;
}

constexpr std::size_t IndexFromVirtualAddress(std::uintptr_t virtual_address, std::size_t shift_bits) {
	return static_cast<std::size_t>((virtual_address >> shift_bits) & IndexMaskForLevel());
}

bool IsPageAligned(std::uintptr_t address) {
	return (address & kPageOffsetMask) == 0;
}

constexpr std::size_t ShiftBitsForLevel(std::size_t level) {
	return Rocinante::Memory::Paging::kPageShiftBits + (level * Rocinante::Memory::Paging::kIndexBitsPerLevel);
}

std::size_t IndexFromVirtualAddressAtLevel(std::uintptr_t virtual_address, std::size_t level) {
	return IndexFromVirtualAddress(virtual_address, ShiftBitsForLevel(level));
}

std::uint64_t CacheBitsForMode(CacheMode mode) {
	return (static_cast<std::uint64_t>(mode) << PteBits::kCacheShift) & PteBits::kCacheMask;
}

std::uint64_t PrivilegeLevelKernelBits() {
	// PLV=0 is kernel in the LoongArch privileged spec.
	static constexpr std::uint64_t kKernelPrivilegeLevel = 0;
	return (kKernelPrivilegeLevel << PteBits::kPrivilegeLevelShift) & PteBits::kPrivilegeLevelMask;
}

std::uint64_t LeafFlagsForPermissions(PagePermissions permissions) {
	std::uint64_t flags = 0;

	// V (Valid) indicates a valid translation exists.
	flags |= PteBits::kValid;
	// P (Physical page exists) is used during page walking.
	flags |= PteBits::kPresent;
	flags |= PrivilegeLevelKernelBits();
	flags |= CacheBitsForMode(permissions.cache);

	if (permissions.global) {
		flags |= PteBits::kGlobal;
	}

	if (permissions.access == AccessPermissions::ReadWrite) {
		flags |= PteBits::kWrite;
		flags |= PteBits::kDirty;
	}

	if (permissions.execute == ExecutePermissions::NoExecute) {
		flags |= PteBits::kNoExecute;
	}

	return flags;
}

std::uint64_t EncodeLeafEntry(std::uintptr_t physical_page_base, std::uint64_t physical_page_base_mask, PagePermissions permissions) {
	// Store the aligned physical page base in the high bits, and OR in flags.
	//
	// IMPORTANT:
	// This encoding is chosen to be straightforward and unit-testable.
	// We have not yet validated the full hardware page-table-walker requirements
	// for non-leaf entries (intermediate levels).
	return (static_cast<std::uint64_t>(physical_page_base) & physical_page_base_mask) | LeafFlagsForPermissions(permissions);
}

std::uint64_t EncodeTablePointer(std::uintptr_t physical_page_base, std::uint64_t physical_page_base_mask) {
	// For non-leaf page-table entries that point to a next-level table, encode:
	// - aligned physical base
	// - V=1, P=1
	// - bit 6 clear (not a huge-page entry)
	return (static_cast<std::uint64_t>(physical_page_base) & physical_page_base_mask) | PteBits::kValid | PteBits::kPresent;
}

bool EntryIsPresent(std::uint64_t entry) {
	// Treat "present" as "walkable": require both the P and V fields.
	return (entry & (PteBits::kPresent | PteBits::kValid)) == (PteBits::kPresent | PteBits::kValid);
}

bool TableIsEmpty(const PageTablePage* table) {
	if (!table) return false;
	for (std::uint64_t entry : table->entries) {
		if (EntryIsPresent(entry)) return false;
	}
	return true;
}

std::uintptr_t EntryPhysicalPageBase(std::uint64_t entry, std::uint64_t physical_page_base_mask) {
	return static_cast<std::uintptr_t>(entry & physical_page_base_mask);
}

PageTablePage* PageTablePageFromPhysical(std::uintptr_t physical_page_base) {
	if (!IsMappedAddressTranslationMode()) {
		// Direct-address mode: physical address equals (low bits of) virtual address.
		return reinterpret_cast<PageTablePage*>(physical_page_base);
	}

	// Mapped mode: page-table pages must be accessed through a mapped virtual
	// address. Rocinante policy is a higher-half linear physmap.
	const Rocinante::Memory::PagingState* paging_state = Rocinante::Memory::TryGetPagingState();
	const std::uint8_t virtual_address_bits = paging_state
		? paging_state->address_bits.virtual_address_bits
		: static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits());
	const std::uintptr_t physmap_virtual =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(physical_page_base, virtual_address_bits);
	return reinterpret_cast<PageTablePage*>(physmap_virtual);
}

const PageTablePage* PageTablePageFromPhysicalConst(std::uintptr_t physical_page_base) {
	if (!IsMappedAddressTranslationMode()) {
		return reinterpret_cast<const PageTablePage*>(physical_page_base);
	}

	const Rocinante::Memory::PagingState* paging_state = Rocinante::Memory::TryGetPagingState();
	const std::uint8_t virtual_address_bits = paging_state
		? paging_state->address_bits.virtual_address_bits
		: static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits());
	const std::uintptr_t physmap_virtual =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(physical_page_base, virtual_address_bits);
	return reinterpret_cast<const PageTablePage*>(physmap_virtual);
}

bool EnsureNextLevelTable(
	PhysicalMemoryManager* pmm,
	PageTablePage* current_table,
	std::size_t index,
	PageTablePage** out_next_table,
	std::uint64_t physical_page_base_mask
) {
	if (!pmm || !current_table || !out_next_table) return false;

	std::uint64_t entry = current_table->entries[index];
	if (EntryIsPresent(entry)) {
		// This software builder/walker does not support huge pages yet.
		// In the privileged spec, bit 6 set in a directory entry indicates a huge-page entry.
		if ((entry & PteBits::kGlobal) != 0) return false;
		*out_next_table = PageTablePageFromPhysical(EntryPhysicalPageBase(entry, physical_page_base_mask));
		return true;
	}

	const auto new_table_page = pmm->AllocatePage();
	if (!new_table_page.has_value()) return false;
	const std::uintptr_t new_table_physical_base = new_table_page.value();
	if (!IsPageAligned(new_table_physical_base)) return false;

	auto* new_table = PageTablePageFromPhysical(new_table_physical_base);
	(void)memset(new_table, 0, sizeof(PageTablePage));

	current_table->entries[index] = EncodeTablePointer(new_table_physical_base, physical_page_base_mask);
	*out_next_table = new_table;
	return true;
}

struct Layout final {
	std::uint8_t virtual_address_bits;
	std::uint8_t physical_address_bits;
	std::uint8_t level_count;
	std::uint64_t virtual_address_low_mask;
	std::uint64_t physical_address_mask;
	std::uint64_t physical_page_base_mask;
};

Rocinante::Optional<Layout> BuildLayout(AddressSpaceBits address_bits) {
	if (address_bits.virtual_address_bits == 0) return Rocinante::nullopt;
	if (address_bits.physical_address_bits == 0) return Rocinante::nullopt;
	if (address_bits.virtual_address_bits > 64) return Rocinante::nullopt;
	if (address_bits.physical_address_bits > 64) return Rocinante::nullopt;

	const std::uint8_t level_count = LevelCountFromVirtualAddressBits(address_bits.virtual_address_bits);
	if (level_count == 0) return Rocinante::nullopt;

	const std::uint64_t virtual_address_low_mask = MaskFromBits(address_bits.virtual_address_bits);
	const std::uint64_t physical_address_mask = MaskFromBits(address_bits.physical_address_bits);
	const std::uint64_t physical_page_base_mask = PhysicalPageBaseMaskFromBits(address_bits.physical_address_bits);
	if (physical_page_base_mask == 0) return Rocinante::nullopt;

	return Layout{
		.virtual_address_bits = address_bits.virtual_address_bits,
		.physical_address_bits = address_bits.physical_address_bits,
		.level_count = level_count,
		.virtual_address_low_mask = virtual_address_low_mask,
		.physical_address_mask = physical_address_mask,
		.physical_page_base_mask = physical_page_base_mask,
	};
}

bool ValidateVirtualAddress(std::uintptr_t virtual_address, const Layout& layout) {
	// LoongArch LA64 uses canonical virtual addresses in mapped address translation mode.
	//
	// Given N valid virtual address bits, the CPU expects bits [63:N] to be a sign
	// extension of bit [N-1]. This permits both the lower half (sign bit 0) and
	// higher half (sign bit 1) address spaces.
	//
	// Note: system software can further reduce the effective N via CSR.RVACFG.RDVA.
	// For this software page-table walker, `layout.virtual_address_bits` is treated
	// as the effective valid width.
	if (layout.virtual_address_bits == 0) return false;
	if (layout.virtual_address_bits >= 64) return true;

	const std::uint64_t low_mask = layout.virtual_address_low_mask;
	const std::uint64_t upper_mask = ~low_mask;
	const std::uint64_t sign_bit = (1ull << (layout.virtual_address_bits - 1u));
	const std::uint64_t address = static_cast<std::uint64_t>(virtual_address);
	const bool sign = (address & sign_bit) != 0;
	const std::uint64_t upper = address & upper_mask;
	return sign ? (upper == upper_mask) : (upper == 0);
}

bool ValidatePhysicalAddress(std::uintptr_t physical_address, const Layout& layout) {
	return (static_cast<std::uint64_t>(physical_address) & ~layout.physical_address_mask) == 0;
}

bool FreeAllPageTablesRecursive(
	PhysicalMemoryManager* pmm,
	std::uintptr_t table_physical_base,
	const Layout& layout,
	std::size_t level,
	bool* out_had_global_leaf_mappings
) {
	if (!pmm) return false;
	if (!IsPageAligned(table_physical_base)) return false;

	auto* table = PageTablePageFromPhysical(table_physical_base);
	if (!table) return false;

	// For non-leaf levels, directory entries point at lower-level tables.
	// This walker does not support huge-page directory entries.
	if (level > 0) {
		for (std::size_t i = 0; i < kEntriesPerTable; i++) {
			const std::uint64_t entry = table->entries[i];
			if (!EntryIsPresent(entry)) continue;

			// In the LoongArch privileged spec, bit 6 set in a directory entry
			// indicates a huge-page mapping. We do not support huge pages yet.
			if ((entry & PteBits::kGlobal) != 0) return false;

			const std::uintptr_t child_physical = EntryPhysicalPageBase(entry, layout.physical_page_base_mask);
			if (!FreeAllPageTablesRecursive(pmm, child_physical, layout, level - 1, out_had_global_leaf_mappings)) return false;
			table->entries[i] = 0;
		}
	} else {
		// Leaf PTEs map physical frames. We intentionally do not free those frames
		// here; we only decrement their map_count and free the page-table page.
		for (std::size_t i = 0; i < kEntriesPerTable; i++) {
			const std::uint64_t entry = table->entries[i];
			if (!EntryIsPresent(entry)) continue;

			if (out_had_global_leaf_mappings && ((entry & PteBits::kGlobal) != 0)) {
				*out_had_global_leaf_mappings = true;
			}

			const std::uintptr_t mapped_physical = EntryPhysicalPageBase(entry, layout.physical_page_base_mask);
			if (!pmm->DecrementMapCountForPhysical(mapped_physical)) return false;
			table->entries[i] = 0;
		}
	}
	return pmm->FreePage(table_physical_base);
}

} // namespace

Rocinante::Optional<PageTableRoot> AllocateRootPageTable(PhysicalMemoryManager* pmm) {
	if (!pmm) return Rocinante::nullopt;
	const auto page = pmm->AllocatePage();
	if (!page.has_value()) return Rocinante::nullopt;

	const std::uintptr_t root_physical_base = page.value();
	if (!IsPageAligned(root_physical_base)) return Rocinante::nullopt;

	auto* root = PageTablePageFromPhysical(root_physical_base);
	(void)memset(root, 0, sizeof(PageTablePage));

	return PageTableRoot{.root_physical_address = root_physical_base};
}

bool FreeAllPageTables4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	AddressSpaceBits address_bits
) {
	return FreeAllPageTables4KiB(pmm, root, address_bits, nullptr);
}

bool FreeAllPageTables4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	AddressSpaceBits address_bits,
	bool* out_had_global_leaf_mappings
) {
	if (!pmm) return false;
	if (out_had_global_leaf_mappings) *out_had_global_leaf_mappings = false;
	const auto layout_opt = BuildLayout(address_bits);
	if (!layout_opt.has_value()) return false;
	const Layout layout = layout_opt.value();
	if (layout.level_count == 0) return false;
	if (layout.level_count > kMaxSupportedLevelCount) return false;

	if (root.root_physical_address == 0) return false;
	if (!IsPageAligned(root.root_physical_address)) return false;

	return FreeAllPageTablesRecursive(
		pmm,
		root.root_physical_address,
		layout,
		static_cast<std::size_t>(layout.level_count - 1),
		out_had_global_leaf_mappings);
}

bool MapRange4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_base,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	PagePermissions permissions
) {
	return MapRange4KiB(
		pmm,
		root,
		virtual_base,
		physical_base,
		size_bytes,
		permissions,
		AddressSpaceBitsFromCPUCFG()
	);
}

bool MapRange4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_base,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	PagePermissions permissions,
	AddressSpaceBits address_bits
) {
	if (!pmm) return false;
	const auto layout_opt = BuildLayout(address_bits);
	if (!layout_opt.has_value()) return false;
	const Layout layout = layout_opt.value();

	if (!IsPageAligned(virtual_base)) return false;
	if (!IsPageAligned(physical_base)) return false;
	if ((size_bytes % kPageSizeBytes) != 0) return false;
	if (!ValidateVirtualAddress(virtual_base, layout)) return false;
	if (!ValidatePhysicalAddress(physical_base, layout)) return false;

	for (std::size_t offset = 0; offset < size_bytes; offset += kPageSizeBytes) {
		const std::uintptr_t virtual_address = virtual_base + offset;
		const std::uintptr_t physical_address = physical_base + offset;
		if (!MapPage4KiB(pmm, root, virtual_address, physical_address, permissions, address_bits)) {
			return false;
		}
	}

	return true;
}

bool MapPage4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_address,
	std::uintptr_t physical_address,
	PagePermissions permissions
) {
	return MapPage4KiB(
		pmm,
		root,
		virtual_address,
		physical_address,
		permissions,
		AddressSpaceBitsFromCPUCFG()
	);
}

bool MapPage4KiB(
	PhysicalMemoryManager* pmm,
	const PageTableRoot& root,
	std::uintptr_t virtual_address,
	std::uintptr_t physical_address,
	PagePermissions permissions,
	AddressSpaceBits address_bits
) {
	if (!pmm) return false;
	const auto layout_opt = BuildLayout(address_bits);
	if (!layout_opt.has_value()) return false;
	const Layout layout = layout_opt.value();

	if (root.root_physical_address == 0) return false;
	if (!ValidateVirtualAddress(virtual_address, layout)) return false;
	if (!ValidatePhysicalAddress(physical_address, layout)) return false;
	if (!IsPageAligned(virtual_address)) return false;
	if (!IsPageAligned(physical_address)) return false;

	auto* table = PageTablePageFromPhysical(root.root_physical_address);
	if (!table) return false;

	for (auto level = static_cast<std::size_t>(layout.level_count - 1); level > 0; level--) {
		const std::size_t index = IndexFromVirtualAddressAtLevel(virtual_address, level);
		PageTablePage* next_table = nullptr;
		if (!EnsureNextLevelTable(pmm, table, index, &next_table, layout.physical_page_base_mask)) return false;
		table = next_table;
		if (!table) return false;
	}

	const std::size_t leaf_index = IndexFromVirtualAddressAtLevel(virtual_address, 0);
	if (EntryIsPresent(table->entries[leaf_index])) return false;

	table->entries[leaf_index] = EncodeLeafEntry(physical_address, layout.physical_page_base_mask, permissions);
	if (!IsPhysMapLeafMapping(virtual_address, physical_address, address_bits)) {
		if (!pmm->IncrementMapCountForPhysical(physical_address)) {
			table->entries[leaf_index] = 0;
			return false;
		}
	}
	return true;
}

bool UnmapPage4KiB(PhysicalMemoryManager* pmm, const PageTableRoot& root, std::uintptr_t virtual_address) {
	return UnmapPage4KiB(
		pmm,
		root,
		virtual_address,
		AddressSpaceBitsFromCPUCFG()
	);
}

bool UnmapPage4KiB(PhysicalMemoryManager* pmm, const PageTableRoot& root, std::uintptr_t virtual_address, AddressSpaceBits address_bits) {
	if (!pmm) return false;
	const auto layout_opt = BuildLayout(address_bits);
	if (!layout_opt.has_value()) return false;
	const Layout layout = layout_opt.value();

	if (root.root_physical_address == 0) return false;
	if (!ValidateVirtualAddress(virtual_address, layout)) return false;
	if (!IsPageAligned(virtual_address)) return false;

	if (layout.level_count > kMaxSupportedLevelCount) return false;

	PageTablePage* tables_by_level[kMaxSupportedLevelCount] = {};
	std::uintptr_t physical_by_level[kMaxSupportedLevelCount] = {};
	std::size_t child_index_by_level[kMaxSupportedLevelCount] = {};

	auto* table = PageTablePageFromPhysical(root.root_physical_address);
	if (!table) return false;
	const auto root_level = static_cast<std::size_t>(layout.level_count - 1);
	tables_by_level[root_level] = table;
	physical_by_level[root_level] = root.root_physical_address;

	for (auto level = static_cast<std::size_t>(layout.level_count - 1); level > 0; level--) {
		const std::size_t index = IndexFromVirtualAddressAtLevel(virtual_address, level);
		child_index_by_level[level] = index;
		const std::uint64_t entry = table->entries[index];
		// This software walker does not yet support huge pages. In the LoongArch
		// privileged spec, bit 6 being set in a directory entry indicates a huge-page mapping.
		if ((entry & PteBits::kGlobal) != 0) return false;
		if (!EntryIsPresent(entry)) return false;
		const std::uintptr_t next_physical = EntryPhysicalPageBase(entry, layout.physical_page_base_mask);
		table = PageTablePageFromPhysical(next_physical);
		if (!table) return false;
		tables_by_level[level - 1] = table;
		physical_by_level[level - 1] = next_physical;
	}

	const std::size_t leaf_index = IndexFromVirtualAddressAtLevel(virtual_address, 0);
	const std::uint64_t leaf_entry = table->entries[leaf_index];
	if (!EntryIsPresent(leaf_entry)) return false;
	const std::uintptr_t mapped_physical = EntryPhysicalPageBase(leaf_entry, layout.physical_page_base_mask);
	if (!IsPhysMapLeafMapping(virtual_address, mapped_physical, address_bits)) {
		if (!pmm->DecrementMapCountForPhysical(mapped_physical)) return false;
	}
	table->entries[leaf_index] = 0;

	// Policy: reclaim empty intermediate page-table pages.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - INVTLB maintains consistency between page tables and the TLB.
	//   Callers that rely on immediate hardware enforcement must invalidate stale
	//   TLB entries after changing page tables.
	for (std::size_t level = 0; (level + 1) < layout.level_count; level++) {
		PageTablePage* current = tables_by_level[level];
		if (!current) return false;
		if (!TableIsEmpty(current)) break;

		const std::uintptr_t current_physical = physical_by_level[level];
		if (current_physical == 0) return false;
		if (!pmm->FreePage(current_physical)) return false;

		PageTablePage* parent = tables_by_level[level + 1];
		if (!parent) return false;
		parent->entries[child_index_by_level[level + 1]] = 0;
	}
	return true;
}

Rocinante::Optional<std::uintptr_t> Translate(const PageTableRoot& root, std::uintptr_t virtual_address) {
	return Translate(
		root,
		virtual_address,
		AddressSpaceBitsFromCPUCFG()
	);
}

Rocinante::Optional<std::uintptr_t> Translate(const PageTableRoot& root, std::uintptr_t virtual_address, AddressSpaceBits address_bits) {
	const auto layout_opt = BuildLayout(address_bits);
	if (!layout_opt.has_value()) return Rocinante::nullopt;
	const Layout layout = layout_opt.value();

	if (root.root_physical_address == 0) return Rocinante::nullopt;
	if (!ValidateVirtualAddress(virtual_address, layout)) return Rocinante::nullopt;

	const auto* table = PageTablePageFromPhysicalConst(root.root_physical_address);
	if (!table) return Rocinante::nullopt;

	for (auto level = static_cast<std::size_t>(layout.level_count - 1); level > 0; level--) {
		const std::size_t index = IndexFromVirtualAddressAtLevel(virtual_address, level);
		const std::uint64_t entry = table->entries[index];
		// This software walker does not yet support huge pages. In the LoongArch
		// privileged spec, bit 6 being set in a directory entry indicates a huge-page mapping.
		if ((entry & PteBits::kGlobal) != 0) return Rocinante::nullopt;
		if (!EntryIsPresent(entry)) return Rocinante::nullopt;
		table = PageTablePageFromPhysicalConst(EntryPhysicalPageBase(entry, layout.physical_page_base_mask));
		if (!table) return Rocinante::nullopt;
	}

	const std::size_t leaf_index = IndexFromVirtualAddressAtLevel(virtual_address, 0);
	const std::uint64_t page_offset = static_cast<std::uint64_t>(virtual_address & kPageOffsetMask);
	const std::uint64_t pte_entry = table->entries[leaf_index];
	if (!EntryIsPresent(pte_entry)) return Rocinante::nullopt;

	const std::uintptr_t physical_page_base = EntryPhysicalPageBase(pte_entry, layout.physical_page_base_mask);
	return physical_page_base + static_cast<std::uintptr_t>(page_offset);
}

} // namespace Rocinante::Memory::Paging

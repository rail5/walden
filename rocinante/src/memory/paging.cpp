/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "paging.h"

#include <src/memory/pmm.h>
#include <src/sp/cpucfg.h>

extern "C" void* memset(void* destination, int byte_value, std::size_t byte_count);

namespace Rocinante::Memory::Paging {

namespace {

// Software-walker masking policy:
//
// Our PTE encoding places flags in the high bits (e.g. No-Execute at bit 62).
// When extracting the physical address, we must mask out those flags.
//
static constexpr std::uint8_t kMaxSupportedLevelCount = 6; // (64 - 12 + 8) / 9 = 6 for 4 KiB pages.

constexpr std::uint8_t BitIndexFromSingleBitMask(std::uint64_t mask) {
	std::uint8_t index = 0;
	while (((mask >> index) & 0x1ull) == 0) {
		index++;
	}
	return index;
}

static constexpr std::uint8_t kLowestHighFlagBit =
	(BitIndexFromSingleBitMask(PteBits::kNoRead) < BitIndexFromSingleBitMask(PteBits::kNoExecute))
		? BitIndexFromSingleBitMask(PteBits::kNoRead)
		: BitIndexFromSingleBitMask(PteBits::kNoExecute);

// Highest PALEN we can encode without colliding with the high-bit flags we use.
// If PALEN == kLowestHighFlagBit, the highest physical address bit is
// (PALEN-1) == (kLowestHighFlagBit-1), which is safe.
static constexpr std::uint8_t kMaxEncodablePhysicalAddressBits = kLowestHighFlagBit;

static Rocinante::Memory::Paging::AddressSpaceBits AddressSpaceBitsFromCPUCFG() {
	const std::uint32_t virtual_address_bits = Rocinante::GetCPUCFG().VirtualAddressBits();
	const std::uint32_t physical_address_bits = Rocinante::GetCPUCFG().PhysicalAddressBits();
	return Rocinante::Memory::Paging::AddressSpaceBits{
		.virtual_address_bits = static_cast<std::uint8_t>(virtual_address_bits),
		.physical_address_bits = static_cast<std::uint8_t>(physical_address_bits),
	};
}

static std::uint64_t MaskFromBits(std::uint8_t bits) {
	if (bits >= 64) return ~0ull;
	if (bits == 0) return 0;
	return (1ull << bits) - 1ull;
}

static std::uint8_t LevelCountFromVirtualAddressBits(std::uint8_t virtual_address_bits) {
	const std::uint32_t offset_bits = static_cast<std::uint32_t>(Rocinante::Memory::Paging::kPageShiftBits);
	const std::uint32_t index_bits_per_level = static_cast<std::uint32_t>(Rocinante::Memory::Paging::kIndexBitsPerLevel);

	const std::uint32_t indexable_bits = (virtual_address_bits > offset_bits) ? (virtual_address_bits - offset_bits) : 0u;
	std::uint32_t level_count = (indexable_bits + index_bits_per_level - 1u) / index_bits_per_level;
	if (level_count == 0) level_count = 1;
	if (level_count > kMaxSupportedLevelCount) return 0;
	return static_cast<std::uint8_t>(level_count);
}

static std::uint64_t PhysicalPageBaseMaskFromBits(std::uint8_t physical_address_bits) {
	if (physical_address_bits > kMaxEncodablePhysicalAddressBits) return 0;
	if (physical_address_bits < Rocinante::Memory::Paging::kPageShiftBits) {
		return 0;
	}
	const std::uint64_t physical_address_mask = MaskFromBits(physical_address_bits);
	return physical_address_mask & Rocinante::Memory::Paging::kPageBaseMask;
}

static constexpr std::uint64_t IndexMaskForLevel() {
	return (1ull << kIndexBitsPerLevel) - 1ull;
}

static constexpr std::size_t IndexFromVirtualAddress(std::uintptr_t virtual_address, std::size_t shift_bits) {
	return static_cast<std::size_t>((virtual_address >> shift_bits) & IndexMaskForLevel());
}

static bool IsPageAligned(std::uintptr_t address) {
	return (address & kPageOffsetMask) == 0;
}

static constexpr std::size_t ShiftBitsForLevel(std::size_t level) {
	return Rocinante::Memory::Paging::kPageShiftBits + (level * Rocinante::Memory::Paging::kIndexBitsPerLevel);
}

static std::size_t IndexFromVirtualAddressAtLevel(std::uintptr_t virtual_address, std::size_t level) {
	return IndexFromVirtualAddress(virtual_address, ShiftBitsForLevel(level));
}

static std::uint64_t CacheBitsForMode(CacheMode mode) {
	return (static_cast<std::uint64_t>(mode) << PteBits::kCacheShift) & PteBits::kCacheMask;
}

static std::uint64_t PrivilegeLevelKernelBits() {
	// PLV=0 is kernel in the LoongArch privileged spec.
	static constexpr std::uint64_t kKernelPrivilegeLevel = 0;
	return (kKernelPrivilegeLevel << PteBits::kPrivilegeLevelShift) & PteBits::kPrivilegeLevelMask;
}

static std::uint64_t LeafFlagsForPermissions(PagePermissions permissions) {
	std::uint64_t flags = 0;

	// For early bring-up we treat "present" and "valid" together.
	flags |= PteBits::kPresent;
	flags |= PteBits::kValid;
	flags |= PrivilegeLevelKernelBits();
	flags |= CacheBitsForMode(permissions.cache);

	if (permissions.global) {
		flags |= PteBits::kGlobal;
	}

	if (permissions.access == AccessPermissions::ReadWrite) {
		flags |= PteBits::kWrite;
		flags |= PteBits::kDirty;
		flags |= PteBits::kModified;
	}

	if (permissions.execute == ExecutePermissions::NoExecute) {
		flags |= PteBits::kNoExecute;
	}

	return flags;
}

static std::uint64_t EncodeLeafEntry(std::uintptr_t physical_page_base, std::uint64_t physical_page_base_mask, PagePermissions permissions) {
	// Store the aligned physical page base in the high bits, and OR in flags.
	//
	// IMPORTANT:
	// This encoding is chosen to be straightforward and unit-testable.
	// We have not yet validated the full hardware page-table-walker requirements
	// for non-leaf entries (intermediate levels).
	return (static_cast<std::uint64_t>(physical_page_base) & physical_page_base_mask) | LeafFlagsForPermissions(permissions);
}

static std::uint64_t EncodeTablePointer(std::uintptr_t physical_page_base, std::uint64_t physical_page_base_mask) {
	// For now we encode intermediate pointers as "present + valid" plus the
	// aligned physical base.
	//
	// Flaw / bring-up note:
	// We still need to confirm whether LoongArch hardware expects intermediate
	// entries to have the same flag semantics as leaf PTEs.
	return (static_cast<std::uint64_t>(physical_page_base) & physical_page_base_mask) | PteBits::kPresent | PteBits::kValid;
}

static bool EntryIsPresent(std::uint64_t entry) {
	return (entry & PteBits::kPresent) != 0;
}

static std::uintptr_t EntryPhysicalPageBase(std::uint64_t entry, std::uint64_t physical_page_base_mask) {
	return static_cast<std::uintptr_t>(entry & physical_page_base_mask);
}

static PageTablePage* PageTablePageFromPhysical(std::uintptr_t physical_page_base) {
	return reinterpret_cast<PageTablePage*>(physical_page_base);
}

static const PageTablePage* PageTablePageFromPhysicalConst(std::uintptr_t physical_page_base) {
	return reinterpret_cast<const PageTablePage*>(physical_page_base);
}

static bool EnsureNextLevelTable(
	PhysicalMemoryManager* pmm,
	PageTablePage* current_table,
	std::size_t index,
	PageTablePage** out_next_table,
	std::uint64_t physical_page_base_mask
) {
	if (!pmm || !current_table || !out_next_table) return false;

	std::uint64_t entry = current_table->entries[index];
	if (EntryIsPresent(entry)) {
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

static Rocinante::Optional<Layout> BuildLayout(AddressSpaceBits address_bits) {
	if (address_bits.virtual_address_bits == 0) return {};
	if (address_bits.physical_address_bits == 0) return {};
	if (address_bits.virtual_address_bits > 64) return {};
	if (address_bits.physical_address_bits > 64) return {};

	const std::uint8_t level_count = LevelCountFromVirtualAddressBits(address_bits.virtual_address_bits);
	if (level_count == 0) return {};

	const std::uint64_t virtual_address_low_mask = MaskFromBits(address_bits.virtual_address_bits);
	const std::uint64_t physical_address_mask = MaskFromBits(address_bits.physical_address_bits);
	const std::uint64_t physical_page_base_mask = PhysicalPageBaseMaskFromBits(address_bits.physical_address_bits);
	if (physical_page_base_mask == 0) return {};

	return Layout{
		.virtual_address_bits = address_bits.virtual_address_bits,
		.physical_address_bits = address_bits.physical_address_bits,
		.level_count = level_count,
		.virtual_address_low_mask = virtual_address_low_mask,
		.physical_address_mask = physical_address_mask,
		.physical_page_base_mask = physical_page_base_mask,
	};
}

static bool ValidateVirtualAddress(std::uintptr_t virtual_address, const Layout& layout) {
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
	const std::uint64_t sign_bit = (1ull << (layout.virtual_address_bits - 1));
	const std::uint64_t address = static_cast<std::uint64_t>(virtual_address);
	const bool sign = (address & sign_bit) != 0;
	const std::uint64_t upper = address & upper_mask;
	return sign ? (upper == upper_mask) : (upper == 0);
}

static bool ValidatePhysicalAddress(std::uintptr_t physical_address, const Layout& layout) {
	return (static_cast<std::uint64_t>(physical_address) & ~layout.physical_address_mask) == 0;
}

} // namespace

Rocinante::Optional<PageTableRoot> AllocateRootPageTable(PhysicalMemoryManager* pmm) {
	if (!pmm) return {};
	const auto page = pmm->AllocatePage();
	if (!page.has_value()) return {};

	const std::uintptr_t root_physical_base = page.value();
	if (!IsPageAligned(root_physical_base)) return {};

	auto* root = PageTablePageFromPhysical(root_physical_base);
	(void)memset(root, 0, sizeof(PageTablePage));

	return PageTableRoot{.root_physical_address = root_physical_base};
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

	for (std::size_t level = static_cast<std::size_t>(layout.level_count - 1); level > 0; level--) {
		const std::size_t index = IndexFromVirtualAddressAtLevel(virtual_address, level);
		PageTablePage* next_table = nullptr;
		if (!EnsureNextLevelTable(pmm, table, index, &next_table, layout.physical_page_base_mask)) return false;
		table = next_table;
		if (!table) return false;
	}

	const std::size_t leaf_index = IndexFromVirtualAddressAtLevel(virtual_address, 0);
	if (EntryIsPresent(table->entries[leaf_index])) return false;

	table->entries[leaf_index] = EncodeLeafEntry(physical_address, layout.physical_page_base_mask, permissions);
	return true;
}

bool UnmapPage4KiB(const PageTableRoot& root, std::uintptr_t virtual_address) {
	return UnmapPage4KiB(
		root,
		virtual_address,
		AddressSpaceBitsFromCPUCFG()
	);
}

bool UnmapPage4KiB(const PageTableRoot& root, std::uintptr_t virtual_address, AddressSpaceBits address_bits) {
	const auto layout_opt = BuildLayout(address_bits);
	if (!layout_opt.has_value()) return false;
	const Layout layout = layout_opt.value();

	if (root.root_physical_address == 0) return false;
	if (!ValidateVirtualAddress(virtual_address, layout)) return false;
	if (!IsPageAligned(virtual_address)) return false;

	auto* table = PageTablePageFromPhysical(root.root_physical_address);
	if (!table) return false;

	for (std::size_t level = static_cast<std::size_t>(layout.level_count - 1); level > 0; level--) {
		const std::size_t index = IndexFromVirtualAddressAtLevel(virtual_address, level);
		const std::uint64_t entry = table->entries[index];
		if (!EntryIsPresent(entry)) return false;
		table = PageTablePageFromPhysical(EntryPhysicalPageBase(entry, layout.physical_page_base_mask));
		if (!table) return false;
	}

	const std::size_t leaf_index = IndexFromVirtualAddressAtLevel(virtual_address, 0);
	if (!EntryIsPresent(table->entries[leaf_index])) return false;
	table->entries[leaf_index] = 0;
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
	if (!layout_opt.has_value()) return {};
	const Layout layout = layout_opt.value();

	if (root.root_physical_address == 0) return {};
	if (!ValidateVirtualAddress(virtual_address, layout)) return {};

	const auto* table = PageTablePageFromPhysicalConst(root.root_physical_address);
	if (!table) return {};

	for (std::size_t level = static_cast<std::size_t>(layout.level_count - 1); level > 0; level--) {
		const std::size_t index = IndexFromVirtualAddressAtLevel(virtual_address, level);
		const std::uint64_t entry = table->entries[index];
		if (!EntryIsPresent(entry)) return {};
		table = PageTablePageFromPhysicalConst(EntryPhysicalPageBase(entry, layout.physical_page_base_mask));
		if (!table) return {};
	}

	const std::size_t leaf_index = IndexFromVirtualAddressAtLevel(virtual_address, 0);
	const std::uint64_t page_offset = static_cast<std::uint64_t>(virtual_address & kPageOffsetMask);
	const std::uint64_t pte_entry = table->entries[leaf_index];
	if (!EntryIsPresent(pte_entry)) return {};

	const std::uintptr_t physical_page_base = EntryPhysicalPageBase(pte_entry, layout.physical_page_base_mask);
	return physical_page_base + static_cast<std::uintptr_t>(page_offset);
}

} // namespace Rocinante::Memory::Paging

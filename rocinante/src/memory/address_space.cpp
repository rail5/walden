/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "address_space.h"

#include <src/memory/paging_hw.h>
#include <src/memory/pmm.h>

namespace Rocinante::Memory {

Rocinante::Optional<AddressSpace> AddressSpace::Create(
	PhysicalMemoryManager* physical_memory_manager,
	Paging::AddressSpaceBits address_bits,
	std::uint16_t address_space_id
) {
	if (!physical_memory_manager) return Rocinante::nullopt;
	const auto root_or = Paging::AllocateRootPageTable(physical_memory_manager);
	if (!root_or.has_value()) return Rocinante::nullopt;
	return AddressSpace(root_or.value(), address_bits, address_space_id);
}

bool AddressSpace::MapPage4KiB(
	PhysicalMemoryManager* physical_memory_manager,
	std::uintptr_t virtual_address,
	std::uintptr_t physical_address,
	Paging::PagePermissions permissions
) const {
	return Paging::MapPage4KiB(
		physical_memory_manager,
		low_half_root_,
		virtual_address,
		physical_address,
		permissions,
		address_bits_);
}

bool AddressSpace::MapRange4KiB(
	PhysicalMemoryManager* physical_memory_manager,
	std::uintptr_t virtual_base,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	Paging::PagePermissions permissions
) const {
	return Paging::MapRange4KiB(
		physical_memory_manager,
		low_half_root_,
		virtual_base,
		physical_base,
		size_bytes,
		permissions,
		address_bits_);
}

bool AddressSpace::UnmapPage4KiB(PhysicalMemoryManager* physical_memory_manager, std::uintptr_t virtual_address) const {
	return Paging::UnmapPage4KiB(physical_memory_manager, low_half_root_, virtual_address, address_bits_);
}

bool AddressSpace::DestroyPageTables(PhysicalMemoryManager* physical_memory_manager) {
	if (!physical_memory_manager) return false;
	if (low_half_root_.root_physical_address == 0) return false;

	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 4.2.4.7 (INVTLB): INVTLB maintains consistency between the TLB
	//   and page table data in memory.
	//
	// Bring-up policy:
	// - We do not yet have a per-ASID invalidation helper exposed.
	// - Destroying an address space implies its translations must not be used.
	// - Use coarse INVTLB op=0 (flush all) before freeing page-table pages.
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	if (!Paging::FreeAllPageTables4KiB(physical_memory_manager, low_half_root_, address_bits_)) {
		return false;
	}

	// Poison the root to make accidental reuse fail loudly.
	low_half_root_.root_physical_address = 0;
	return true;
}

} // namespace Rocinante::Memory

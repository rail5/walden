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

class PhysicalMemoryManager;

/**
 * @brief A minimal address-space object.
 *
 * Scope (bring-up):
 * - Own a low-half page-table root and an ASID (address-space identifier).
 * - Provide mapping helpers for 4 KiB pages/ranges using existing Paging APIs.
 *
 * Non-goals (explicit for now):
 * - No VMA model (regions, permissions policy, accounting).
 * - No user/kernel split policy.
 * - No teardown that reclaims intermediate page tables.
 * - No SMP safety.
 */
class AddressSpace final {
public:
	static Rocinante::Optional<AddressSpace> Create(
		PhysicalMemoryManager* physical_memory_manager,
		Paging::AddressSpaceBits address_bits,
		std::uint16_t address_space_id);

	const Paging::PageTableRoot& LowHalfRoot() const { return low_half_root_; }
	Paging::AddressSpaceBits AddressBits() const { return address_bits_; }
	std::uint16_t AddressSpaceId() const { return address_space_id_; }

	bool MapPage4KiB(
		PhysicalMemoryManager* physical_memory_manager,
		std::uintptr_t virtual_address,
		std::uintptr_t physical_address,
		Paging::PagePermissions permissions) const;

	bool MapRange4KiB(
		PhysicalMemoryManager* physical_memory_manager,
		std::uintptr_t virtual_base,
		std::uintptr_t physical_base,
		std::size_t size_bytes,
		Paging::PagePermissions permissions) const;

	bool UnmapPage4KiB(std::uintptr_t virtual_address) const;

private:
	AddressSpace(Paging::PageTableRoot low_half_root, Paging::AddressSpaceBits address_bits, std::uint16_t address_space_id)
		: low_half_root_(low_half_root), address_bits_(address_bits), address_space_id_(address_space_id) {
	}

	Paging::PageTableRoot low_half_root_{};
	Paging::AddressSpaceBits address_bits_{};
	std::uint16_t address_space_id_ = 0;
};

} // namespace Rocinante::Memory

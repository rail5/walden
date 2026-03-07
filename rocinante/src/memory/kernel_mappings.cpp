/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/memory/kernel_mappings.h>

#include <src/memory/kernel_va_allocator.h>
#include <src/memory/pmm.h>

namespace Rocinante::Memory::KernelMappings {

namespace {

static bool IsPageAligned(std::uintptr_t address) {
	return (address % Rocinante::Memory::Paging::kPageSizeBytes) == 0;
}

} // namespace

Rocinante::Optional<MappedRange> MapPhysicalRange4KiB(
	PhysicalMemoryManager* pmm,
	const Paging::PageTableRoot& root,
	KernelVirtualAddressAllocator* va_allocator,
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	Paging::PagePermissions permissions,
	Paging::AddressSpaceBits address_bits
) {
	if (!pmm || !va_allocator) return Rocinante::nullopt;
	if (size_bytes == 0) return Rocinante::nullopt;
	if (!IsPageAligned(physical_base)) return Rocinante::nullopt;
	if ((size_bytes % Rocinante::Memory::Paging::kPageSizeBytes) != 0) return Rocinante::nullopt;

	const auto virtual_base_or = va_allocator->Allocate(size_bytes, Rocinante::Memory::Paging::kPageSizeBytes);
	if (!virtual_base_or.has_value()) return Rocinante::nullopt;
	const std::uintptr_t virtual_base = virtual_base_or.value();

	std::size_t mapped_bytes = 0;
	while (mapped_bytes < size_bytes) {
		const std::uintptr_t virtual_page = virtual_base + mapped_bytes;
		const std::uintptr_t physical_page = physical_base + mapped_bytes;

		if (!Paging::MapPage4KiB(
			pmm,
			root,
			virtual_page,
			physical_page,
			permissions,
			address_bits)) {
			break;
		}

		mapped_bytes += Rocinante::Memory::Paging::kPageSizeBytes;
	}

	if (mapped_bytes != size_bytes) {
		std::size_t unmapped_bytes = 0;
		while (unmapped_bytes < mapped_bytes) {
			const std::uintptr_t virtual_page = virtual_base + unmapped_bytes;
			(void)Paging::UnmapPage4KiB(root, virtual_page, address_bits);
			unmapped_bytes += Rocinante::Memory::Paging::kPageSizeBytes;
		}
		(void)va_allocator->Free(virtual_base, size_bytes);
		return Rocinante::nullopt;
	}

	return MappedRange{.virtual_base = virtual_base, .size_bytes = size_bytes};
}

bool UnmapAndFree4KiB(
	const Paging::PageTableRoot& root,
	KernelVirtualAddressAllocator* va_allocator,
	std::uintptr_t virtual_base,
	std::size_t size_bytes,
	Paging::AddressSpaceBits address_bits
) {
	if (!va_allocator) return false;
	if (size_bytes == 0) return false;
	if (!IsPageAligned(virtual_base)) return false;
	if ((size_bytes % Rocinante::Memory::Paging::kPageSizeBytes) != 0) return false;

	bool all_unmapped = true;
	std::size_t unmapped_bytes = 0;
	while (unmapped_bytes < size_bytes) {
		const std::uintptr_t virtual_page = virtual_base + unmapped_bytes;
		if (!Paging::UnmapPage4KiB(root, virtual_page, address_bits)) {
			all_unmapped = false;
		}
		unmapped_bytes += Rocinante::Memory::Paging::kPageSizeBytes;
	}

	if (!all_unmapped) return false;
	return va_allocator->Free(virtual_base, size_bytes);
}

} // namespace Rocinante::Memory::KernelMappings

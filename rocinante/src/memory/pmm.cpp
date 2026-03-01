/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "pmm.h"

#include <src/memory/virtual_layout.h>
#include <src/sp/cpucfg.h>

namespace Rocinante::Memory {

namespace {

// LoongArch privileged architecture: CSR.CRMD (Current Mode Information).
//
// Spec anchor:
// - LoongArch-Vol1-EN.html, Section 5.2 (Virtual Address Space and Address Translation Mode)
//   - CRMD.DA=1, CRMD.PG=0 => direct address translation mode
//   - CRMD.DA=0, CRMD.PG=1 => mapped address translation mode
namespace Csr {
	static constexpr std::uint32_t kCurrentModeInformation = 0x0; // CSR.CRMD
}

namespace CurrentModeInformation {
	static constexpr std::uint64_t kDirectAddressingEnable = (1ull << 3); // CRMD.DA
	static constexpr std::uint64_t kPagingEnable = (1ull << 4);           // CRMD.PG
}

static inline std::uint64_t ReadCurrentModeInformation() {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(Csr::kCurrentModeInformation));
	return value;
}

static inline bool IsMappedAddressTranslationMode() {
	const std::uint64_t crmd = ReadCurrentModeInformation();
	const bool direct_addressing = (crmd & CurrentModeInformation::kDirectAddressingEnable) != 0;
	const bool paging = (crmd & CurrentModeInformation::kPagingEnable) != 0;
	return (!direct_addressing) && paging;
}

static constexpr std::uintptr_t AlignDown(std::uintptr_t value, std::size_t alignment) {
	return value & ~(static_cast<std::uintptr_t>(alignment) - 1);
}

static constexpr std::uintptr_t AlignUp(std::uintptr_t value, std::size_t alignment) {
	return (value + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1);
}

static constexpr bool AddOverflows(std::uintptr_t a, std::size_t b) {
	const std::uintptr_t sum = a + static_cast<std::uintptr_t>(b);
	return sum < a;
}

static bool RangesOverlap(
	std::uintptr_t a_base,
	std::size_t a_size,
	std::uintptr_t b_base,
	std::size_t b_size
) {
	if (a_size == 0 || b_size == 0) return false;
	if (AddOverflows(a_base, a_size)) return true;
	if (AddOverflows(b_base, b_size)) return true;
	const std::uintptr_t a_end = a_base + static_cast<std::uintptr_t>(a_size);
	const std::uintptr_t b_end = b_base + static_cast<std::uintptr_t>(b_size);
	return (a_base < b_end) && (b_base < a_end);
}

} // namespace

PhysicalMemoryManager& GetPhysicalMemoryManager() {
	static PhysicalMemoryManager instance;
	return instance;
}

void PhysicalMemoryManager::_reset_state() {
	m_bitmap_physical_base = 0;
	m_bitmap_size_bytes = 0;
	m_tracked_physical_base = 0;
	m_tracked_physical_limit = 0;
	m_page_count = 0;
	m_free_page_count = 0;
	m_next_search_index = 0;
	m_initialized = false;
}

std::uint8_t* PhysicalMemoryManager::_bitmap_ptr() {
	if (m_bitmap_physical_base == 0 || m_bitmap_size_bytes == 0) return nullptr;

	if (!IsMappedAddressTranslationMode()) {
		return reinterpret_cast<std::uint8_t*>(m_bitmap_physical_base);
	}

	const std::uint8_t virtual_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits());
	const std::uintptr_t physmap_virtual =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(m_bitmap_physical_base, virtual_address_bits);
	return reinterpret_cast<std::uint8_t*>(physmap_virtual);
}

const std::uint8_t* PhysicalMemoryManager::_bitmap_ptr() const {
	if (m_bitmap_physical_base == 0 || m_bitmap_size_bytes == 0) return nullptr;

	if (!IsMappedAddressTranslationMode()) {
		return reinterpret_cast<const std::uint8_t*>(m_bitmap_physical_base);
	}

	const std::uint8_t virtual_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits());
	const std::uintptr_t physmap_virtual =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(m_bitmap_physical_base, virtual_address_bits);
	return reinterpret_cast<const std::uint8_t*>(physmap_virtual);
}

bool PhysicalMemoryManager::_allocate_bitmap(
	const BootMemoryMap& boot_map,
	std::size_t page_count,
	std::uintptr_t kernel_physical_base,
	std::uintptr_t kernel_physical_end,
	std::uintptr_t device_tree_physical_base,
	std::size_t device_tree_size_bytes
) {
	if (page_count == 0) return false;

	// Bitmap encoding:
	// - 1 bit per tracked physical page.
	// - bit = 1 means "used / not allocatable".
	// - bit = 0 means "free / allocatable".
	const std::size_t bit_count = page_count;
	const std::size_t byte_count = (bit_count + 7) / 8;
	if (byte_count == 0) return false;
	const std::size_t byte_count_aligned = (byte_count + 15u) & ~static_cast<std::size_t>(15u);

	// Heap-free bitmap allocation.
	//
	// We carve the bitmap storage out of a UsableRAM region described by the boot
	// memory map. This keeps the PMM self-contained and avoids requiring any
	// dynamic allocator for its own metadata.
	std::uintptr_t chosen_base = 0;
	for (std::size_t i = 0; i < boot_map.region_count; i++) {
		const auto& r = boot_map.regions[i];
		if (r.type != BootMemoryRegion::Type::UsableRAM) continue;
		if (r.size_bytes == 0) continue;
		if (AddOverflows(static_cast<std::uintptr_t>(r.physical_base), static_cast<std::size_t>(r.size_bytes))) continue;

		const std::uintptr_t region_begin = static_cast<std::uintptr_t>(r.physical_base);
		const std::uintptr_t region_end = region_begin + static_cast<std::uintptr_t>(r.size_bytes);
		std::uintptr_t candidate = AlignUp(region_begin, 16);
		if (candidate < region_begin) continue;

		// If the region begins under the kernel/DTB reservations (common in tests
		// and some boot environments), advance to the end of the overlapping range
		// and retry within the same UsableRAM region.
		for (std::size_t bump = 0; bump < 4; bump++) {
			if (candidate < region_begin) break;
			if (AddOverflows(candidate, byte_count_aligned)) break;
			const std::uintptr_t candidate_end = candidate + static_cast<std::uintptr_t>(byte_count_aligned);
			if (candidate_end > region_end) break;

			bool bumped = false;
			if (kernel_physical_end > kernel_physical_base) {
				const std::size_t kernel_size = static_cast<std::size_t>(kernel_physical_end - kernel_physical_base);
				if (RangesOverlap(candidate, byte_count_aligned, kernel_physical_base, kernel_size)) {
					candidate = AlignUp(kernel_physical_end, 16);
					bumped = true;
				}
			}

			if (!bumped && device_tree_physical_base != 0 && device_tree_size_bytes != 0) {
				if (RangesOverlap(candidate, byte_count_aligned, device_tree_physical_base, device_tree_size_bytes)) {
					if (AddOverflows(device_tree_physical_base, device_tree_size_bytes)) break;
					const std::uintptr_t dtb_end = device_tree_physical_base + static_cast<std::uintptr_t>(device_tree_size_bytes);
					candidate = AlignUp(dtb_end, 16);
					bumped = true;
				}
			}

			if (!bumped) {
				chosen_base = candidate;
				break;
			}
		}

		if (chosen_base != 0) break;
	}

	if (chosen_base == 0) return false;

	m_bitmap_physical_base = chosen_base;
	m_bitmap_size_bytes = byte_count_aligned;
	std::uint8_t* bitmap = _bitmap_ptr();
	if (!bitmap) return false;

	// Default to "used" for everything.
	//
	// Policy note: pages not explicitly described as UsableRAM are treated as
	// non-allocatable.
	for (std::size_t i = 0; i < byte_count_aligned; i++) {
		bitmap[i] = 0xFF;
	}

	return true;
}

bool PhysicalMemoryManager::_is_page_used(std::size_t page_index) const {
	const std::size_t byte_index = page_index / 8;
	const std::size_t bit_index = page_index % 8;
	const std::uint8_t* bitmap = _bitmap_ptr();
	if (!bitmap) return true;
	return (bitmap[byte_index] & (1u << bit_index)) != 0;
}

void PhysicalMemoryManager::_set_page_used(std::size_t page_index) {
	const std::size_t byte_index = page_index / 8;
	const std::size_t bit_index = page_index % 8;
	std::uint8_t* bitmap = _bitmap_ptr();
	if (!bitmap) return;
	bitmap[byte_index] |= static_cast<std::uint8_t>(1u << bit_index);
}

void PhysicalMemoryManager::_set_page_free(std::size_t page_index) {
	const std::size_t byte_index = page_index / 8;
	const std::size_t bit_index = page_index % 8;
	std::uint8_t* bitmap = _bitmap_ptr();
	if (!bitmap) return;
	bitmap[byte_index] &= static_cast<std::uint8_t>(~(1u << bit_index));
}

std::uintptr_t PhysicalMemoryManager::_page_index_to_physical(std::size_t page_index) const {
	return m_tracked_physical_base + (page_index * kPageSizeBytes);
}

std::size_t PhysicalMemoryManager::_physical_to_page_index(std::uintptr_t physical_address) const {
	return static_cast<std::size_t>((physical_address - m_tracked_physical_base) / kPageSizeBytes);
}

bool PhysicalMemoryManager::_physical_range_to_page_indices(
	std::uintptr_t physical_base,
	std::size_t size_bytes,
	std::size_t* out_index_begin,
	std::size_t* out_index_end
) const {
	if (!out_index_begin || !out_index_end) return false;
	if (size_bytes == 0) return false;
	if (AddOverflows(physical_base, size_bytes)) return false;

	const std::uintptr_t physical_end = physical_base + size_bytes;

	// Clamp to our tracked range.
	if (physical_end <= m_tracked_physical_base) return false;
	if (physical_base >= m_tracked_physical_limit) return false;

	const std::uintptr_t clamped_begin = (physical_base < m_tracked_physical_base) ? m_tracked_physical_base : physical_base;
	const std::uintptr_t clamped_end = (physical_end > m_tracked_physical_limit) ? m_tracked_physical_limit : physical_end;
	if (clamped_end <= clamped_begin) return false;

	const std::uintptr_t aligned_begin = AlignDown(clamped_begin, kPageSizeBytes);
	const std::uintptr_t aligned_end = AlignUp(clamped_end, kPageSizeBytes);
	if (aligned_end <= aligned_begin) return false;

	const std::size_t index_begin = _physical_to_page_index(aligned_begin);
	const std::size_t index_end = _physical_to_page_index(aligned_end);
	if (index_begin >= index_end) return false;
	if (index_end > m_page_count) return false;

	*out_index_begin = index_begin;
	*out_index_end = index_end;
	return true;
}

bool PhysicalMemoryManager::_mark_range_free(std::uintptr_t physical_base, std::size_t size_bytes) {
	std::size_t index_begin = 0;
	std::size_t index_end = 0;
	if (!_physical_range_to_page_indices(physical_base, size_bytes, &index_begin, &index_end)) return true;

	for (std::size_t i = index_begin; i < index_end; i++) {
		_set_page_free(i);
	}
	return true;
}

bool PhysicalMemoryManager::_mark_range_used(std::uintptr_t physical_base, std::size_t size_bytes) {
	std::size_t index_begin = 0;
	std::size_t index_end = 0;
	if (!_physical_range_to_page_indices(physical_base, size_bytes, &index_begin, &index_end)) return true;

	for (std::size_t i = index_begin; i < index_end; i++) {
		_set_page_used(i);
	}
	return true;
}

bool PhysicalMemoryManager::InitializeFromBootMemoryMap(
	const BootMemoryMap& boot_map,
	std::uintptr_t kernel_physical_base,
	std::uintptr_t kernel_physical_end,
	std::uintptr_t device_tree_physical_base,
	std::size_t device_tree_size_bytes
) {
	_reset_state();

	// Determine the physical span of UsableRAM.
	bool saw_usable = false;
	std::uintptr_t usable_min = 0;
	std::uintptr_t usable_max = 0;

	for (std::size_t i = 0; i < boot_map.region_count; i++) {
		const auto& r = boot_map.regions[i];
		if (r.type != BootMemoryRegion::Type::UsableRAM) continue;
		if (r.size_bytes == 0) continue;
		if (AddOverflows(static_cast<std::uintptr_t>(r.physical_base), static_cast<std::size_t>(r.size_bytes))) continue;

		const std::uintptr_t begin = static_cast<std::uintptr_t>(r.physical_base);
		const std::uintptr_t end = begin + static_cast<std::uintptr_t>(r.size_bytes);

		if (!saw_usable) {
			usable_min = begin;
			usable_max = end;
			saw_usable = true;
		} else {
			if (begin < usable_min) usable_min = begin;
			if (end > usable_max) usable_max = end;
		}
	}

	if (!saw_usable) return false;

	m_tracked_physical_base = AlignDown(usable_min, kPageSizeBytes);
	m_tracked_physical_limit = AlignUp(usable_max, kPageSizeBytes);
	if (m_tracked_physical_limit <= m_tracked_physical_base) return false;

	m_page_count = static_cast<std::size_t>((m_tracked_physical_limit - m_tracked_physical_base) / kPageSizeBytes);
	if (m_page_count == 0) return false;

	if (!_allocate_bitmap(
		boot_map,
		m_page_count,
		kernel_physical_base,
		kernel_physical_end,
		device_tree_physical_base,
		device_tree_size_bytes
	)) {
		_reset_state();
		return false;
	}

	// 1) Mark all UsableRAM pages free.
	for (std::size_t i = 0; i < boot_map.region_count; i++) {
		const auto& r = boot_map.regions[i];
		if (r.type != BootMemoryRegion::Type::UsableRAM) continue;
		if (!_mark_range_free(static_cast<std::uintptr_t>(r.physical_base), static_cast<std::size_t>(r.size_bytes))) {
			_reset_state();
			return false;
		}
	}

	// 2) Apply Reserved regions from the DTB (reserved wins over usable).
	for (std::size_t i = 0; i < boot_map.region_count; i++) {
		const auto& r = boot_map.regions[i];
		if (r.type != BootMemoryRegion::Type::Reserved) continue;
		if (!_mark_range_used(static_cast<std::uintptr_t>(r.physical_base), static_cast<std::size_t>(r.size_bytes))) {
			_reset_state();
			return false;
		}
	}

	// 3) Proactively reserve the kernel image range.
	if (kernel_physical_end > kernel_physical_base) {
		if (!_mark_range_used(kernel_physical_base, static_cast<std::size_t>(kernel_physical_end - kernel_physical_base))) {
			_reset_state();
			return false;
		}
	}

	// 4) Proactively reserve the DTB blob range.
	if (device_tree_physical_base != 0 && device_tree_size_bytes != 0) {
		if (!_mark_range_used(device_tree_physical_base, device_tree_size_bytes)) {
			_reset_state();
			return false;
		}
	}

	// 4.5) Reserve the bitmap storage pages.
	if (m_bitmap_physical_base != 0 && m_bitmap_size_bytes != 0) {
		if (!_mark_range_used(m_bitmap_physical_base, m_bitmap_size_bytes)) {
			_reset_state();
			return false;
		}
	}

	// 5) Proactively reserve the zero page.
	//
	// Why:
	// - The kernel frequently uses 0 as a sentinel for "invalid physical address".
	// - Some early bring-up code (page tables, scratch buffers) dereferences
	//   physical addresses directly while CRMD.DA=1, so allocating physical 0
	//   would imply dereferencing a null pointer.
	//
	// Policy:
	// Treat physical page [0, 4KiB) as permanently reserved if it is within the
	// tracked range.
	(void)_mark_range_used(0, kPageSizeBytes);

	// Finalize free-page accounting.
	std::size_t free_pages = 0;
	for (std::size_t i = 0; i < m_page_count; i++) {
		if (!_is_page_used(i)) free_pages++;
	}
	m_free_page_count = free_pages;
	m_next_search_index = 0;
	m_initialized = true;
	return true;
}

Rocinante::Optional<std::uintptr_t> PhysicalMemoryManager::AllocatePage() {
	if (!m_initialized) return Rocinante::nullopt;
	if (m_free_page_count == 0) return Rocinante::nullopt;

	for (std::size_t scan = 0; scan < m_page_count; scan++) {
		const std::size_t index = (m_next_search_index + scan) % m_page_count;
		if (_is_page_used(index)) continue;

		_set_page_used(index);
		m_free_page_count--;
		m_next_search_index = (index + 1) % m_page_count;
		return Rocinante::Optional<std::uintptr_t>(_page_index_to_physical(index));
	}

	return Rocinante::nullopt;
}

bool PhysicalMemoryManager::FreePage(std::uintptr_t physical_address) {
	if (!m_initialized) return false;
	if ((physical_address % kPageSizeBytes) != 0) return false;
	if (physical_address < m_tracked_physical_base) return false;
	if (physical_address >= m_tracked_physical_limit) return false;

	const std::size_t index = _physical_to_page_index(physical_address);
	if (index >= m_page_count) return false;

	if (!_is_page_used(index)) {
		// Double-free or memory corruption.
		return false;
	}

	_set_page_free(index);
	m_free_page_count++;
	if (index < m_next_search_index) m_next_search_index = index;
	return true;
}

bool PhysicalMemoryManager::ReserveRange(std::uintptr_t physical_base, std::size_t size_bytes) {
	if (!m_initialized) return false;

	std::size_t index_begin = 0;
	std::size_t index_end = 0;
	if (!_physical_range_to_page_indices(physical_base, size_bytes, &index_begin, &index_end)) return true;

	for (std::size_t i = index_begin; i < index_end; i++) {
		if (_is_page_used(i)) continue;
		_set_page_used(i);
		m_free_page_count--;
	}

	return true;
}

} // namespace Rocinante::Memory

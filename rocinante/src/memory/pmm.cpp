/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "pmm.h"

#include "heap.h"

namespace Rocinante::Memory {

namespace {

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

} // namespace

PhysicalMemoryManager& GetPhysicalMemoryManager() {
	static PhysicalMemoryManager instance;
	return instance;
}

void PhysicalMemoryManager::_reset_state() {
	if (m_bitmap) {
		Rocinante::Heap::Free(m_bitmap);
	}
	m_bitmap = nullptr;
	m_bitmap_size_bytes = 0;
	m_tracked_physical_base = 0;
	m_tracked_physical_limit = 0;
	m_page_count = 0;
	m_free_page_count = 0;
	m_next_search_index = 0;
	m_initialized = false;
}

bool PhysicalMemoryManager::_allocate_bitmap(std::size_t page_count) {
	if (page_count == 0) return false;

	// Bitmap encoding:
	// - 1 bit per tracked physical page.
	// - bit = 1 means "used / not allocatable".
	// - bit = 0 means "free / allocatable".
	const std::size_t bit_count = page_count;
	const std::size_t byte_count = (bit_count + 7) / 8;
	if (byte_count == 0) return false;

	// Bootstrap heap-backed bitmap allocation.
	//
	// This is safe because the bootstrap heap lives inside the kernel image
	// range, which the PMM will reserve before any future allocator uses it.
	void* bitmap = Rocinante::Heap::Alloc(byte_count, 16);
	if (!bitmap) return false;

	m_bitmap = static_cast<std::uint8_t*>(bitmap);
	m_bitmap_size_bytes = byte_count;

	// Default to "used" for everything.
	//
	// Policy note: pages not explicitly described as UsableRAM are treated as
	// non-allocatable.
	for (std::size_t i = 0; i < byte_count; i++) {
		m_bitmap[i] = 0xFF;
	}

	return true;
}

bool PhysicalMemoryManager::_is_page_used(std::size_t page_index) const {
	const std::size_t byte_index = page_index / 8;
	const std::size_t bit_index = page_index % 8;
	return (m_bitmap[byte_index] & (1u << bit_index)) != 0;
}

void PhysicalMemoryManager::_set_page_used(std::size_t page_index) {
	const std::size_t byte_index = page_index / 8;
	const std::size_t bit_index = page_index % 8;
	m_bitmap[byte_index] |= static_cast<std::uint8_t>(1u << bit_index);
}

void PhysicalMemoryManager::_set_page_free(std::size_t page_index) {
	const std::size_t byte_index = page_index / 8;
	const std::size_t bit_index = page_index % 8;
	m_bitmap[byte_index] &= static_cast<std::uint8_t>(~(1u << bit_index));
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

	if (!_allocate_bitmap(m_page_count)) {
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

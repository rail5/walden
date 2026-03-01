/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>
#include <src/memory/boot_memory_map.h>

namespace Rocinante::Memory {

/**
 * @brief Physical Memory Manager (PMM) for page-frame allocation.
 *
 * This is the first "real" memory manager component in the kernel.
 * It consumes the boot-reported memory map (DTB today, UEFI later) and provides
 * a page-granular allocator over physical RAM.
 *
 * Key policy:
 * - Pages not explicitly described as UsableRAM are treated as non-allocatable.
 * - BootMemoryRegion::Type::Reserved always wins.
 * - The kernel image range and the DTB blob range are proactively reserved.
 *
 * Current limitations (intentional for early bring-up):
 * - Not SMP-safe (no locking).
 * - Linear scan allocation (no freelists / buddy allocator yet).
 * - Tracks only the span of UsableRAM it was initialized with.
 */
class PhysicalMemoryManager final {
	public:
		// 4 KiB is the common base page size for LoongArch; huge pages come later.
		static constexpr std::size_t kPageSizeBytes = 4096;

		PhysicalMemoryManager() = default;
		PhysicalMemoryManager(const PhysicalMemoryManager&) = delete;
		PhysicalMemoryManager& operator=(const PhysicalMemoryManager&) = delete;

		bool IsInitialized() const { return m_initialized; }

		/**
		 * @brief Initializes the PMM from a boot memory map.
		 *
		 * Args:
		 * - boot_map: Regions parsed from DTB (or later UEFI).
		 * - kernel_physical_base/kernel_physical_end: inclusive-exclusive physical
		 *   range of the kernel image and all early static storage that must never
		 *   be allocated (stacks, bootstrap heap buffer, etc.).
		 * - device_tree_physical_base/device_tree_size_bytes: the DTB blob range.
		 *
		 * Returns: true on success.
		 */
		bool InitializeFromBootMemoryMap(
			const BootMemoryMap& boot_map,
			std::uintptr_t kernel_physical_base,
			std::uintptr_t kernel_physical_end,
			std::uintptr_t device_tree_physical_base,
			std::size_t device_tree_size_bytes
		);

		// Allocates one physical page and returns its physical address.
		Rocinante::Optional<std::uintptr_t> AllocatePage();

		// Frees a page previously returned by AllocatePage().
		bool FreePage(std::uintptr_t physical_address);

		// Explicitly reserves a physical range (marks its pages non-allocatable).
		bool ReserveRange(std::uintptr_t physical_base, std::size_t size_bytes);

		std::size_t TotalPages() const { return m_page_count; }
		std::size_t FreePages() const { return m_free_page_count; }

		std::uintptr_t TrackedPhysicalBase() const { return m_tracked_physical_base; }
		std::uintptr_t TrackedPhysicalLimit() const { return m_tracked_physical_limit; }

	private:
		std::uintptr_t m_bitmap_physical_base = 0;
		std::size_t m_bitmap_size_bytes = 0;

		std::uintptr_t m_tracked_physical_base = 0;
		std::uintptr_t m_tracked_physical_limit = 0;

		std::size_t m_page_count = 0;
		std::size_t m_free_page_count = 0;
		std::size_t m_next_search_index = 0;
		bool m_initialized = false;

		std::uint8_t* _bitmap_ptr();
		const std::uint8_t* _bitmap_ptr() const;

		bool _allocate_bitmap(
			const BootMemoryMap& boot_map,
			std::size_t page_count,
			std::uintptr_t kernel_physical_base,
			std::uintptr_t kernel_physical_end,
			std::uintptr_t device_tree_physical_base,
			std::size_t device_tree_size_bytes
		);
		void _reset_state();

		bool _mark_range_free(std::uintptr_t physical_base, std::size_t size_bytes);
		bool _mark_range_used(std::uintptr_t physical_base, std::size_t size_bytes);

		bool _physical_range_to_page_indices(
			std::uintptr_t physical_base,
			std::size_t size_bytes,
			std::size_t* out_index_begin,
			std::size_t* out_index_end
		) const;

		bool _is_page_used(std::size_t page_index) const;
		void _set_page_used(std::size_t page_index);
		void _set_page_free(std::size_t page_index);

		std::uintptr_t _page_index_to_physical(std::size_t page_index) const;
		std::size_t _physical_to_page_index(std::uintptr_t physical_address) const;
};

// Returns the single canonical PMM instance for the kernel.
PhysicalMemoryManager& GetPhysicalMemoryManager();

} // namespace Rocinante::Memory

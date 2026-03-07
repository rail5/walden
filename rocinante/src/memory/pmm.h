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

		// Per-frame metadata size.
		//
		// Why this is a fixed constant:
		// - The PMM must reserve a deterministic amount of physical memory for its
		//   own metadata.
		// - Tests should be able to compute expected metadata footprint without
		//   reaching into private implementation details.
		static constexpr std::size_t kPageFrameMetadataSizeBytes = 16;

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

		/**
		 * @brief Converts a page-aligned physical address into a page frame number (PFN).
		 *
		 * Returns nullopt if:
		 * - the PMM is not initialized,
		 * - the address is not page-aligned, or
		 * - the address is outside the PMM tracked physical span.
		 */
		Rocinante::Optional<std::size_t> PageFrameNumberFromPhysical(std::uintptr_t physical_address) const;

		/**
		 * @brief Converts a PFN into a page-aligned physical address.
		 *
		 * Returns nullopt if:
		 * - the PMM is not initialized, or
		 * - the PFN is outside the tracked span.
		 */
		Rocinante::Optional<std::uintptr_t> PhysicalFromPageFrameNumber(std::size_t page_frame_number) const;

		/**
		 * @brief Increments the leaf-mapping count (map_count) for a physical page.
		 *
		 * Semantics:
		 * - If the physical page is within the PMM tracked span, its map_count is
		 *   incremented.
		 * - If the physical page is outside the tracked span (e.g., MMIO), this is
		 *   a no-op and returns true.
		 */
		bool IncrementMapCountForPhysical(std::uintptr_t physical_page_base);

		/**
		 * @brief Decrements the leaf-mapping count (map_count) for a physical page.
		 *
		 * Semantics:
		 * - If the physical page is within the PMM tracked span, its map_count is
		 *   decremented.
		 * - If the physical page is outside the tracked span (e.g., MMIO), this is
		 *   a no-op and returns true.
		 */
		bool DecrementMapCountForPhysical(std::uintptr_t physical_page_base);

		/**
		 * @brief Returns the current map_count for a physical page, if tracked.
		 */
		Rocinante::Optional<std::uint32_t> MapCountForPhysical(std::uintptr_t physical_page_base) const;

	private:
		struct PageFrameMetadata final {
			std::uint32_t ref_count = 0;
			std::uint32_t map_count = 0;
			std::uint32_t flags = 0;
			std::uint32_t reserved = 0;
		};
		static_assert(sizeof(PageFrameMetadata) == kPageFrameMetadataSizeBytes);

		std::uintptr_t m_bitmap_physical_base = 0;
		std::size_t m_bitmap_size_bytes = 0;

		std::uintptr_t m_frame_metadata_physical_base = 0;
		std::size_t m_frame_metadata_size_bytes = 0;

		std::uintptr_t m_tracked_physical_base = 0;
		std::uintptr_t m_tracked_physical_limit = 0;

		std::size_t m_page_count = 0;
		std::size_t m_free_page_count = 0;
		std::size_t m_next_search_index = 0;
		bool m_initialized = false;

		std::uint8_t* _bitmap_ptr();
		const std::uint8_t* _bitmap_ptr() const;
		PageFrameMetadata* _frame_metadata_ptr();
		const PageFrameMetadata* _frame_metadata_ptr() const;

		bool _allocate_bitmap(
			const BootMemoryMap& boot_map,
			std::size_t page_count,
			std::uintptr_t kernel_physical_base,
			std::uintptr_t kernel_physical_end,
			std::uintptr_t device_tree_physical_base,
			std::size_t device_tree_size_bytes
		);
		bool _allocate_frame_metadata(
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

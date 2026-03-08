/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>
#include <src/memory/pmm.h>
#include <src/memory/virtual_layout.h>
#include <src/sp/cpucfg.h>

namespace Rocinante::Memory {

/**
 * @brief Anonymous VM object backing store.
 *
 * - This object owns physical frames for an anonymous mapping.
 * - Frames are indexed by page offset (0 == first 4 KiB page in the object).
 * - On first access to an offset, the object allocates one PMM page and keeps
 *   its `ref_count` until the object releases it.
 *
 * Non-goals / bring-up limitations:
 * - No copy-on-write, no sharing, no file backing.
 * - No sharing / fork semantics.
 *
 * Data structure:
 * - Multi-level paged radix directory.
 * - Directory pages and leaf block pages are allocated on demand from the PMM.
 * - The radix height grows as needed based on the maximum page offset touched.
 */
class AnonymousVmObject final {
	public:
		struct GetOrCreateFrameResult final {
			std::uintptr_t physical_page_base;
			bool created;
		};

		AnonymousVmObject() = default;
		AnonymousVmObject(const AnonymousVmObject&) = delete;
		AnonymousVmObject& operator=(const AnonymousVmObject&) = delete;

		std::size_t PageCount() const { return m_payload_frame_count; }
		bool IsEmpty() const { return m_payload_frame_count == 0; }

		/**
		 * @brief Returns the physical frame backing `page_offset`, allocating on demand.
		 *
		 * Args:
		 * - pmm: the PMM used for frame allocation.
		 * - page_offset: 0-based page index within the object (units: 4 KiB pages).
		 */
		Rocinante::Optional<GetOrCreateFrameResult> GetOrCreateFrameForPageOffset(
			PhysicalMemoryManager* pmm,
			std::size_t page_offset
		) {
			if (!pmm) return Rocinante::nullopt;
			static constexpr std::size_t kIndexBitsPerLevel = 9;
			static constexpr std::size_t kEntriesPerRadixPage = 512;
			static constexpr std::size_t kBlockMask = kEntriesPerRadixPage - 1;
			static_assert((kEntriesPerRadixPage & (kEntriesPerRadixPage - 1)) == 0);

			const std::size_t block_index = page_offset >> kIndexBitsPerLevel;
			const std::size_t offset_in_block = page_offset & kBlockMask;

			if (!EnsureRootDirectoryExists(pmm)) return Rocinante::nullopt;
			if (!EnsureDirectoryHeightForBlockIndex(pmm, block_index)) return Rocinante::nullopt;

			RadixPage* directory = RootDirectoryPage();
			if (!directory) return Rocinante::nullopt;
			if (m_block_index_levels == 0) return Rocinante::nullopt;

			for (std::size_t level = m_block_index_levels; level > 1; level--) {
				const std::size_t shift = (level - 1) * kIndexBitsPerLevel;
				const std::size_t index = (block_index >> shift) & kBlockMask;

				std::uintptr_t child_physical = directory->entries[index];
				if (child_physical == 0) {
					const auto allocated = AllocateAndZeroRadixPage(pmm);
					if (!allocated.has_value()) return Rocinante::nullopt;
					child_physical = allocated.value();
					directory->entries[index] = child_physical;
				}

				directory = RadixPageFromPhysical(child_physical);
				if (!directory) return Rocinante::nullopt;
			}

			const std::size_t leaf_index = block_index & kBlockMask;
			std::uintptr_t block_physical = directory->entries[leaf_index];
			if (block_physical == 0) {
				const auto allocated = AllocateAndZeroRadixPage(pmm);
				if (!allocated.has_value()) return Rocinante::nullopt;
				block_physical = allocated.value();
				directory->entries[leaf_index] = block_physical;
			}

			RadixPage* block = RadixPageFromPhysical(block_physical);
			if (!block) return Rocinante::nullopt;

			const std::uintptr_t existing = block->entries[offset_in_block];
			if (existing != 0) {
				return Rocinante::Optional<GetOrCreateFrameResult>(
					GetOrCreateFrameResult{.physical_page_base = existing, .created = false}
				);
			}

			const auto allocated_frame = pmm->AllocatePage();
			if (!allocated_frame.has_value()) return Rocinante::nullopt;
			const std::uintptr_t physical_page_base = allocated_frame.value();
			block->entries[offset_in_block] = physical_page_base;
			m_payload_frame_count++;

			return Rocinante::Optional<GetOrCreateFrameResult>(
				GetOrCreateFrameResult{.physical_page_base = physical_page_base, .created = true}
			);
		}

		/**
		 * @brief Drops this object's ownership of the frame at `page_offset`.
		 *
		 * Semantics:
		 * - If a frame exists for `page_offset`, clear the mapping and release the
		 *   frame via PMM (`ref_count` decrement).
		 * - If no frame exists, this is a no-op and returns true.
		 *
		 * This does not require paging to be enabled in hardware.
		 */
		bool ReleaseFrameForPageOffset(PhysicalMemoryManager* pmm, std::size_t page_offset) {
			if (!pmm) return false;
			if (m_root_directory_physical == 0) return true;
			if (m_block_index_levels == 0) return false;

			static constexpr std::size_t kIndexBitsPerLevel = 9;
			static constexpr std::size_t kEntriesPerRadixPage = 512;
			static constexpr std::size_t kBlockMask = kEntriesPerRadixPage - 1;
			static_assert((kEntriesPerRadixPage & (kEntriesPerRadixPage - 1)) == 0);

			constexpr std::size_t kMaxDirectoryLevels = (sizeof(std::size_t) * 8 + kIndexBitsPerLevel - 1) / kIndexBitsPerLevel;
			if (m_block_index_levels > kMaxDirectoryLevels) return false;

			const std::size_t block_index = page_offset >> kIndexBitsPerLevel;
			const std::size_t offset_in_block = page_offset & kBlockMask;

			// If the object never grew tall enough to represent this block index, it
			// cannot possibly contain a mapping for it.
			if ((block_index >> (m_block_index_levels * kIndexBitsPerLevel)) != 0) return true;

			std::uintptr_t directory_physical_by_depth[kMaxDirectoryLevels] = {};
			std::size_t index_in_parent_by_depth[kMaxDirectoryLevels] = {};

			std::uintptr_t current_physical = m_root_directory_physical;
			RadixPage* current = RadixPageFromPhysical(current_physical);
			if (!current) return false;
			directory_physical_by_depth[0] = current_physical;
			std::size_t depth = 0;

			for (std::size_t level = m_block_index_levels; level > 1; level--) {
				const std::size_t shift = (level - 1) * kIndexBitsPerLevel;
				const std::size_t index = (block_index >> shift) & kBlockMask;
				const std::uintptr_t child_physical = current->entries[index];
				if (child_physical == 0) return true;

				depth++;
				if (depth >= kMaxDirectoryLevels) return false;
				directory_physical_by_depth[depth] = child_physical;
				index_in_parent_by_depth[depth] = index;

				current_physical = child_physical;
				current = RadixPageFromPhysical(current_physical);
				if (!current) return false;
			}

			const std::size_t leaf_index = block_index & kBlockMask;
			const std::uintptr_t block_physical = current->entries[leaf_index];
			if (block_physical == 0) return true;
			RadixPage* block = RadixPageFromPhysical(block_physical);
			if (!block) return false;

			const std::uintptr_t frame_physical = block->entries[offset_in_block];
			if (frame_physical == 0) return true;

			block->entries[offset_in_block] = 0;
			if (m_payload_frame_count == 0) return false;
			m_payload_frame_count--;

			if (!pmm->ReleasePhysicalPage(frame_physical)) return false;

			// Reclaim empty metadata pages bottom-up. This keeps the object from
			// pinning PMM pages for now-empty internal structures.
			if (RadixPageIsEmpty(block)) {
				if (!pmm->ReleasePhysicalPage(block_physical)) return false;
				current->entries[leaf_index] = 0;

				while (depth > 0) {
					RadixPage* dir = RadixPageFromPhysical(directory_physical_by_depth[depth]);
					if (!dir) return false;
					if (!RadixPageIsEmpty(dir)) break;
					if (!pmm->ReleasePhysicalPage(directory_physical_by_depth[depth])) return false;

					RadixPage* parent = RadixPageFromPhysical(directory_physical_by_depth[depth - 1]);
					if (!parent) return false;
					parent->entries[index_in_parent_by_depth[depth]] = 0;
					depth--;
				}

				RadixPage* root = RadixPageFromPhysical(m_root_directory_physical);
				if (!root) return false;
				if (RadixPageIsEmpty(root)) {
					if (!pmm->ReleasePhysicalPage(m_root_directory_physical)) return false;
					m_root_directory_physical = 0;
					m_block_index_levels = 0;
				}
			}

			return true;
		}

		/**
		 * @brief Releases all frames owned by this object.
		 *
		 * This decrements `ref_count` for each owned frame. Reclamation occurs in
		 * the PMM only when the frame is also not mapped (`map_count == 0`).
		 */
		bool ReleaseAllOwnedFrames(PhysicalMemoryManager* pmm) {
			if (!pmm) return false;
			if (m_root_directory_physical == 0) {
				m_block_index_levels = 0;
				m_payload_frame_count = 0;
				return true;
			}
			if (m_block_index_levels == 0) return false;

			if (!ReleaseDirectorySubtree(pmm, m_root_directory_physical, m_block_index_levels)) {
				return false;
			}

			m_root_directory_physical = 0;
			m_block_index_levels = 0;
			m_payload_frame_count = 0;
			return true;
		}

	private:
		struct alignas(PhysicalMemoryManager::kPageSizeBytes) RadixPage final {
			std::uintptr_t entries[512];
		};
		static_assert(sizeof(RadixPage) == PhysicalMemoryManager::kPageSizeBytes);

		// LoongArch privileged architecture: CSR.CRMD (Current Mode Information).
		//
		// Spec anchor:
		// - LoongArch-Vol1-EN.html, Section 5.2 (Virtual Address Space and Address Translation Mode)
		//   - CRMD.DA=1, CRMD.PG=0 => direct address translation mode
		//   - CRMD.DA=0, CRMD.PG=1 => mapped address translation mode
		struct Csr final {
			static constexpr std::uint32_t kCurrentModeInformation = 0x0; // CSR.CRMD
		};

		struct CurrentModeInformation final {
			static constexpr std::uint64_t kDirectAddressingEnable = (1ull << 3); // CRMD.DA
			static constexpr std::uint64_t kPagingEnable = (1ull << 4);           // CRMD.PG
		};

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

		static RadixPage* RadixPageFromPhysical(std::uintptr_t physical_page_base) {
			if (physical_page_base == 0) return nullptr;
			if (!IsMappedAddressTranslationMode()) {
				return reinterpret_cast<RadixPage*>(physical_page_base);
			}
			const std::uint8_t virtual_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits());
			const std::uintptr_t physmap_virtual =
				Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(physical_page_base, virtual_address_bits);
			return reinterpret_cast<RadixPage*>(physmap_virtual);
		}

		static bool RadixPageIsEmpty(const RadixPage* page) {
			if (!page) return false;
			for (std::size_t i = 0; i < 512; i++) {
				if (page->entries[i] != 0) return false;
			}
			return true;
		}

		static Rocinante::Optional<std::uintptr_t> AllocateAndZeroRadixPage(PhysicalMemoryManager* pmm) {
			if (!pmm) return Rocinante::nullopt;
			const auto allocated = pmm->AllocatePage();
			if (!allocated.has_value()) return Rocinante::nullopt;
			const std::uintptr_t physical_page_base = allocated.value();
			RadixPage* page = RadixPageFromPhysical(physical_page_base);
			if (!page) {
				(void)pmm->ReleasePhysicalPage(physical_page_base);
				return Rocinante::nullopt;
			}
			for (std::size_t i = 0; i < 512; i++) {
				page->entries[i] = 0;
			}
			return Rocinante::Optional<std::uintptr_t>(physical_page_base);
		}

		bool EnsureRootDirectoryExists(PhysicalMemoryManager* pmm) {
			if (m_root_directory_physical != 0) return true;
			const auto allocated = AllocateAndZeroRadixPage(pmm);
			if (!allocated.has_value()) return false;
			m_root_directory_physical = allocated.value();
			m_block_index_levels = 1;
			return true;
		}

		bool EnsureDirectoryHeightForBlockIndex(PhysicalMemoryManager* pmm, std::size_t block_index) {
			if (!pmm) return false;
			if (m_root_directory_physical == 0) return false;
			if (m_block_index_levels == 0) return false;

			// Grow the directory height until it can represent `block_index` in base-512 digits.
			//
			// Kernel policy: we grow by allocating a new root directory and installing
			// the existing root as entry[0]. This is the same growth strategy used by
			// common radix-tree designs (Linux xarray/radix conventions).
			static constexpr std::size_t kIndexBitsPerLevel = 9;
			while ((block_index >> (m_block_index_levels * kIndexBitsPerLevel)) != 0) {
				const auto new_root_allocated = AllocateAndZeroRadixPage(pmm);
				if (!new_root_allocated.has_value()) return false;
				const std::uintptr_t new_root_physical = new_root_allocated.value();
				RadixPage* new_root = RadixPageFromPhysical(new_root_physical);
				if (!new_root) {
					(void)pmm->ReleasePhysicalPage(new_root_physical);
					return false;
				}

				new_root->entries[0] = m_root_directory_physical;
				m_root_directory_physical = new_root_physical;
				m_block_index_levels++;
			}
			return true;
		}

		RadixPage* RootDirectoryPage() const {
			return RadixPageFromPhysical(m_root_directory_physical);
		}

		bool ReleaseDirectorySubtree(PhysicalMemoryManager* pmm, std::uintptr_t directory_physical, std::size_t levels) {
			if (!pmm) return false;
			if (directory_physical == 0) return true;
			if (levels == 0) return false;

			RadixPage* directory = RadixPageFromPhysical(directory_physical);
			if (!directory) return false;

			if (levels == 1) {
				// Leaf directory: entries are block pages. Block pages contain frame pointers.
				for (std::size_t i = 0; i < 512; i++) {
					const std::uintptr_t block_physical = directory->entries[i];
					if (block_physical == 0) continue;

					RadixPage* block = RadixPageFromPhysical(block_physical);
					if (!block) return false;

					for (std::size_t j = 0; j < 512; j++) {
						const std::uintptr_t frame_physical = block->entries[j];
						if (frame_physical == 0) continue;
						if (!pmm->ReleasePhysicalPage(frame_physical)) return false;
					}

					if (!pmm->ReleasePhysicalPage(block_physical)) return false;
				}
				return pmm->ReleasePhysicalPage(directory_physical);
			}

			for (std::size_t i = 0; i < 512; i++) {
				const std::uintptr_t child_physical = directory->entries[i];
				if (child_physical == 0) continue;
				if (!ReleaseDirectorySubtree(pmm, child_physical, levels - 1)) return false;
			}
			return pmm->ReleasePhysicalPage(directory_physical);
		}

		std::uintptr_t m_root_directory_physical = 0;
		std::size_t m_block_index_levels = 0;
		std::size_t m_payload_frame_count = 0;
};

} // namespace Rocinante::Memory

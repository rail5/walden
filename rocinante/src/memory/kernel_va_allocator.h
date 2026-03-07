/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>

namespace Rocinante::Memory {

// Kernel virtual-address region manager (bring-up).
//
// This exists to stop hard-coding ad-hoc virtual placement policy in paging
// bring-up and to provide a foundation for a future kernel VMM.
//
// Spec-backed invariants:
// - The caller must only initialize this allocator with a range of *legal*
//   virtual addresses for the current translation mode (LoongArch-Vol1-EN.html,
//   Section 5.2.3: in mapped address translation mode, "legal" addresses must
//   satisfy VALEN/(VALEN-RDVA) sign-expansion requirements).
// - The caller must also respect the LoongArch split between lower-half and
//   higher-half address spaces when selecting page-walk roots
//   (LoongArch-Vol1-EN.html, Section 7.5.6: higher half is VA[VALEN-1]==1).
//
// Bring-up limitations (intentional flaws):
// - Not thread-safe.
// - Fixed-capacity free-range tracking.
// - No notion of address spaces, ASIDs, or per-process VM.
class KernelVirtualAddressAllocator final {
	static constexpr std::uintptr_t AlignUp(std::uintptr_t value, std::size_t alignment) {
		if (alignment == 0) return value;
		const std::uintptr_t remainder = value % alignment;
		if (remainder == 0) return value;
		return value + (alignment - remainder);
	}

	struct Range final {
		std::uintptr_t base = 0;
		std::uintptr_t limit = 0; // One past end.

		constexpr bool IsValidNonEmpty() const { return base < limit; }
		constexpr std::size_t SizeBytes() const { return static_cast<std::size_t>(limit - base); }
	};

	// Bring-up policy: keep this small but sufficient for early kernel needs.
	//
	// Worst-case splitting behavior:
	// - Each allocation can split one free range into two.
	// - Coalescing during Free can reduce fragmentation over time.
	static constexpr std::size_t kMaxFreeRanges = 32;

	bool m_initialized = false;
	std::uintptr_t m_managed_base = 0;
	std::uintptr_t m_managed_limit = 0;
	Range m_free_ranges[kMaxFreeRanges] = {};
	std::size_t m_free_range_count = 0;

	void RemoveFreeRangeAt(std::size_t index) {
		for (std::size_t i = index + 1; i < m_free_range_count; i++) {
			m_free_ranges[i - 1] = m_free_ranges[i];
		}
		m_free_range_count--;
	}

	bool InsertFreeRangeAt(std::size_t index, Range range) {
		if (m_free_range_count >= kMaxFreeRanges) return false;
		for (std::size_t i = m_free_range_count; i > index; i--) {
			m_free_ranges[i] = m_free_ranges[i - 1];
		}
		m_free_ranges[index] = range;
		m_free_range_count++;
		return true;
	}

	public:
		KernelVirtualAddressAllocator() = default;

		void Init(std::uintptr_t base, std::uintptr_t limit) {
			m_initialized = true;
			m_managed_base = base;
			m_managed_limit = limit;
			m_free_ranges[0] = Range{.base = base, .limit = limit};
			m_free_range_count = (base < limit) ? 1 : 0;
		}

		bool IsInitialized() const {
			return m_initialized;
		}

		std::uintptr_t ManagedBase() const { return m_managed_base; }
		std::uintptr_t ManagedLimit() const { return m_managed_limit; }

		Rocinante::Optional<std::uintptr_t> Allocate(std::size_t size_bytes, std::size_t alignment_bytes) {
			if (!IsInitialized()) return Rocinante::nullopt;
			if (size_bytes == 0) return Rocinante::nullopt;
			if (m_free_range_count == 0) return Rocinante::nullopt;

			for (std::size_t i = 0; i < m_free_range_count; i++) {
				const Range range = m_free_ranges[i];
				if (!range.IsValidNonEmpty()) continue;

				const std::uintptr_t base = AlignUp(range.base, alignment_bytes);
				const std::uintptr_t end = base + static_cast<std::uintptr_t>(size_bytes);
				if (end < base) continue;
				if (base < range.base) continue;
				if (end > range.limit) continue;

				const Range prefix{.base = range.base, .limit = base};
				const Range suffix{.base = end, .limit = range.limit};

				const std::size_t new_count =
					(m_free_range_count - 1) +
					(prefix.IsValidNonEmpty() ? 1 : 0) +
					(suffix.IsValidNonEmpty() ? 1 : 0);
				if (new_count > kMaxFreeRanges) return Rocinante::nullopt;

				RemoveFreeRangeAt(i);
				std::size_t insert_index = i;
				if (prefix.IsValidNonEmpty()) {
					if (!InsertFreeRangeAt(insert_index, prefix)) return Rocinante::nullopt;
					insert_index++;
				}
				if (suffix.IsValidNonEmpty()) {
					if (!InsertFreeRangeAt(insert_index, suffix)) return Rocinante::nullopt;
				}
				return base;
			}

			return Rocinante::nullopt;
		}

		bool Free(std::uintptr_t base, std::size_t size_bytes) {
			if (!IsInitialized()) return false;
			if (size_bytes == 0) return false;
			const std::uintptr_t limit = base + static_cast<std::uintptr_t>(size_bytes);
			if (limit < base) return false;
			if (base < m_managed_base || limit > m_managed_limit) return false;

			Range freed{.base = base, .limit = limit};
			if (!freed.IsValidNonEmpty()) return false;

			// Find insertion point to keep the free list sorted by base.
			std::size_t index = 0;
			while (index < m_free_range_count && m_free_ranges[index].base < freed.base) {
				index++;
			}

			// Reject overlaps (double-free or invalid range).
			if (index != 0 && m_free_ranges[index - 1].limit > freed.base) return false;
			if (index < m_free_range_count && freed.limit > m_free_ranges[index].base) return false;

			if (!InsertFreeRangeAt(index, freed)) return false;

			// Coalesce with previous.
			if (index != 0) {
				Range& prev = m_free_ranges[index - 1];
				Range& cur = m_free_ranges[index];
				if (prev.limit == cur.base) {
					prev.limit = cur.limit;
					RemoveFreeRangeAt(index);
					index--;
				}
			}

			// Coalesce with next.
			if ((index + 1) < m_free_range_count) {
				Range& cur = m_free_ranges[index];
				Range& next = m_free_ranges[index + 1];
				if (cur.limit == next.base) {
					cur.limit = next.limit;
					RemoveFreeRangeAt(index + 1);
				}
			}

			return true;
		}
};

} // namespace Rocinante::Memory

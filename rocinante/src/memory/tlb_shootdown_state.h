/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/sp/atomic.h>

namespace Rocinante::Memory::TlbShootdown {

/**
 * @brief Fixed-width CPU mask for the first SMP TLB shootdown bring-up stage.
 *
 * Bring-up policy:
 * - Represent the online CPU set with one 64-bit mask word.
 * - Therefore this initial implementation can track up to 64 CPUs.
 *
 * Explicit flaws:
 * - If the platform eventually needs more than 64 CPUs, this representation
 *   must change before it can be used as the authoritative online set.
 */
struct CpuMask final {
	static constexpr std::size_t kBitsPerMaskWord = sizeof(std::uint64_t) * 8;
	static constexpr std::size_t kMaxCpuCount = kBitsPerMaskWord;

	std::uint64_t bits = 0;

	static constexpr bool IsRepresentableCoreId(std::uint32_t core_id) {
		return core_id < kMaxCpuCount;
	}

	static constexpr CpuMask ForCore(std::uint32_t core_id) {
		return IsRepresentableCoreId(core_id)
			? CpuMask{.bits = (1ull << core_id)}
			: CpuMask{};
	}

	constexpr bool IsEmpty() const {
		return bits == 0;
	}

	constexpr bool Contains(std::uint32_t core_id) const {
		return IsRepresentableCoreId(core_id) && ((bits & (1ull << core_id)) != 0);
	}

	constexpr bool Add(std::uint32_t core_id) {
		if (!IsRepresentableCoreId(core_id)) return false;
		bits |= (1ull << core_id);
		return true;
	}

	constexpr bool Remove(std::uint32_t core_id) {
		if (!IsRepresentableCoreId(core_id)) return false;
		bits &= ~(1ull << core_id);
		return true;
	}
};

/**
 * @brief Shared generation/ack state for synchronous TLB shootdown.
 *
 * Scope of this first iteration:
 * - Track the online CPU mask.
 * - Allocate monotonically increasing shootdown generations.
 * - Track per-CPU acknowledged generations.
 * - Answer the core completion question: have all target CPUs acknowledged at
 *   least generation N?
 *
 * Spec anchors (LoongArch-Vol1-EN.html):
 * - Section 2.2.7.2 (`AMADD[_DB].D`): atomic fetch-add used for generation allocation.
 * - Section 2.2.7.1 (`AMSWAP[_DB].D`): atomic exchange used to publish ack generations.
 * - Section 2.2.7.3 (`AMCAS[_DB].D`): atomic compare-exchange used to update the online mask.
 * - Section 2.2.7.6 (`LL.ACQ.D`, `SC.REL.D`): acquire/release fallback path used by
 *   `AtomicFetchAddU64AcqRel` when AM* is unavailable.
 * - Section 2.2.8.1 (`DBAR 0`): full load/store barrier semantics used by the `_Db` helpers.
 *
 * Explicit flaws:
 * - This file does not yet model request mailboxes or IPI delivery.
 * - This file does not yet define CPU online/offline lifecycle rules.
 */
class State final {
	volatile std::uint64_t m_next_generation = 0;
	volatile std::uint64_t m_online_cpu_mask_bits = 0;
	volatile std::uint64_t m_ack_generation_by_core[CpuMask::kMaxCpuCount] = {};

	public:
		static constexpr std::uint64_t kNoGeneration = 0;

		State() = default;

		void Reset() {
			Rocinante::AtomicStoreU64Db(&m_next_generation, kNoGeneration);
			Rocinante::AtomicStoreU64Db(&m_online_cpu_mask_bits, 0);
			for (std::size_t core_index = 0; core_index < CpuMask::kMaxCpuCount; core_index++) {
				Rocinante::AtomicStoreU64Db(&m_ack_generation_by_core[core_index], kNoGeneration);
			}
		}

		bool SetCpuOnline(std::uint32_t core_id, bool is_online) {
			if (!CpuMask::IsRepresentableCoreId(core_id)) return false;

			const std::uint64_t core_bit = (1ull << core_id);
			for (;;) {
				std::uint64_t expected = Rocinante::AtomicLoadU64AcqRel(&m_online_cpu_mask_bits);
				const std::uint64_t desired = is_online
					? (expected | core_bit)
					: (expected & ~core_bit);

				if (desired == expected) return true;

				if (Rocinante::AtomicCompareExchangeU64Db(&m_online_cpu_mask_bits, &expected, desired)) {
					return true;
				}
			}
		}

		CpuMask GetOnlineCpuMask() const {
			return CpuMask{.bits = Rocinante::AtomicLoadU64AcqRel(&m_online_cpu_mask_bits)};
		}

		std::uint64_t AllocateGeneration() {
			return Rocinante::AtomicFetchAddU64Db(&m_next_generation, 1) + 1;
		}

		void RecordAcknowledgedGeneration(std::uint32_t core_id, std::uint64_t generation) {
			if (!CpuMask::IsRepresentableCoreId(core_id)) return;
			Rocinante::AtomicStoreU64Db(&m_ack_generation_by_core[core_id], generation);
		}

		std::uint64_t GetAcknowledgedGeneration(std::uint32_t core_id) const {
			if (!CpuMask::IsRepresentableCoreId(core_id)) return kNoGeneration;
			return Rocinante::AtomicLoadU64AcqRel(&m_ack_generation_by_core[core_id]);
		}

		bool HaveAllTargetsAcknowledged(CpuMask target_cpu_mask, std::uint64_t generation) const {
			std::uint64_t remaining_mask = target_cpu_mask.bits;
			for (std::uint32_t core_id = 0; remaining_mask != 0; core_id++) {
				const std::uint64_t current_core_bit = (1ull << core_id);
				if ((remaining_mask & current_core_bit) == 0) continue;

				if (GetAcknowledgedGeneration(core_id) < generation) {
					return false;
				}

				remaining_mask &= ~current_core_bit;
			}

			return true;
		}
};

} // namespace Rocinante::Memory::TlbShootdown

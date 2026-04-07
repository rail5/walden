/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/memory/paging.h>
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

enum class RequestType : std::uint8_t {
	None = 0,
	InvalidateAsid = 1,
	InvalidatePage = 2,
	InvalidateGlobalAll = 3,
};

/**
 * @brief A single TLB shootdown request payload.
 *
 * Bring-up policy:
 * - One request is published into one per-CPU mailbox.
 * - `generation` is the publication token. A value of 0 means "no request".
 * - `virtual_address_page_base` is only meaningful for `InvalidatePage`.
 *
 * Explicit flaws:
 * - This does not yet support batching multiple pages in one mailbox entry.
 * - This does not yet encode permission-downgrade vs unmap intent separately.
 */
struct Request final {
	std::uint64_t generation = 0;
	RequestType type = RequestType::None;
	std::uint16_t address_space_id = 0;
	std::uintptr_t virtual_address_page_base = 0;

	bool IsValid() const {
		if (generation == 0) return false;
		if (type == RequestType::None) return false;

		if (type == RequestType::InvalidatePage) {
			const auto page_offset_mask =
				static_cast<std::uintptr_t>(Rocinante::Memory::Paging::kPageOffsetMask);
			return (virtual_address_page_base & page_offset_mask) == 0;
		}

		return true;
	}
};

/**
 * @brief A published request paired with the target mask sampled for it.
 *
 * Bring-up policy:
 * - The sampled target mask is captured once by the shooter and then treated
 *   as the stable wait-set for that request generation.
 * - CPUs added to or removed from the live online mask afterwards do not alter
 *   this published request's completion condition.
 *
 * Explicit flaws:
 * - This does not yet solve CPU hotplug or scheduler-driven address-space mask
 *   tracking; it only makes the software-only broadcast sampling rule explicit.
 */
struct PublishedRequest final {
	CpuMask target_cpu_mask{};
	Request request{};

	bool IsValid() const {
		return request.IsValid();
	}
};

using RequestHandler = bool (*)(const Request&, void* context);

/**
 * @brief Shared generation/ack state for synchronous TLB shootdown.
 *
 * Scope of this first iteration:
 * - Track the online CPU mask.
 * - Allocate monotonically increasing shootdown generations.
 * - Hold one published request mailbox per CPU.
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
 * - This file models request mailboxes, but not IPI delivery.
 * - This file's current online/offline lifecycle rule is a bring-up policy:
 *   once the online CPU set is frozen, later membership changes are rejected
 *   until a future scheduler / hotplug design replaces that rule.
 * - This file rejects publication into a target mailbox that still has an
 *   unacknowledged generation, but it still does not fully arbitrate multiple
 *   concurrent shooters that race to publish new work at the same time.
 *
 * Memory-ordering rules for this software-only layer:
 * - Shooter, after page-table writes and before request publication:
 *   all request publication stores use `AtomicStoreU64Db()`, which is backed
 *   by `AMSWAP_DB.D` or an LL/SC + `DBAR 0` fallback. This is the ordering
 *   point that makes the already-written PTE state visible before the mailbox
 *   publication token becomes visible.
 * - Shooter, after request publication and before future IPI transport:
 *   the store to `Mailbox::published_generation` is the publication point.
 *   Any future transport layer must not send an IPI before that `_Db` store
 *   has executed.
 * - Shooter, before reusing freed mappings or page-table pages:
 *   this layer requires callers to wait until the sampled request has
 *   completed via `IsPublishedRequestCompleted()` or the equivalent explicit
 *   generation wait before reclaiming state that remote CPUs may still touch.
 * - Target, before reading a mailbox after IPI delivery or polling:
 *   mailbox reads begin with `AtomicLoadU64AcqRel()` of
 *   `Mailbox::published_generation`, so a target that observes a published
 *   generation also observes the request payload written before it.
 * - Target, after local invalidation and before acknowledgement:
 *   the ack path uses `AtomicStoreU64Db()` via `RecordAcknowledgedGeneration()`,
 *   so the acknowledgement cannot become visible before the target's local
 *   invalidation work is ordered before it.
 */
class State final {
	struct Mailbox final {
		volatile std::uint64_t request_type = static_cast<std::uint64_t>(RequestType::None);
		volatile std::uint64_t address_space_id = 0;
		volatile std::uint64_t virtual_address_page_base = 0;
		volatile std::uint64_t published_generation = 0;
	};

	static RequestType DecodeRequestType(std::uint64_t raw_request_type) {
		switch (raw_request_type) {
			case static_cast<std::uint64_t>(RequestType::InvalidateAsid):
				return RequestType::InvalidateAsid;
			case static_cast<std::uint64_t>(RequestType::InvalidatePage):
				return RequestType::InvalidatePage;
			case static_cast<std::uint64_t>(RequestType::InvalidateGlobalAll):
				return RequestType::InvalidateGlobalAll;
			default:
				return RequestType::None;
		}
	}

	bool IsMailboxAvailableForCore(std::uint32_t core_id) const {
		if (!CpuMask::IsRepresentableCoreId(core_id)) return false;

		const Mailbox& mailbox = m_mailbox_by_core[core_id];
		const std::uint64_t published_generation = Rocinante::AtomicLoadU64AcqRel(&mailbox.published_generation);
		if (published_generation == kNoGeneration) return true;

		const std::uint64_t acknowledged_generation = GetAcknowledgedGeneration(core_id);
		return acknowledged_generation >= published_generation;
	}

	bool AreTargetMailboxesAvailable(CpuMask target_cpu_mask) const {
		std::uint64_t remaining_mask = target_cpu_mask.bits;
		for (std::uint32_t core_id = 0; remaining_mask != 0; core_id++) {
			const std::uint64_t current_core_bit = (1ull << core_id);
			if ((remaining_mask & current_core_bit) == 0) continue;

			if (!IsMailboxAvailableForCore(core_id)) {
				return false;
			}

			remaining_mask &= ~current_core_bit;
		}

		return true;
	}

	bool BuildRequest(
		RequestType request_type,
		std::uint16_t address_space_id,
		std::uintptr_t virtual_address_page_base,
		Request* out_request) {
		if (!out_request) return false;

		const Request request{
			.generation = AllocateGeneration(),
			.type = request_type,
			.address_space_id = address_space_id,
			.virtual_address_page_base = virtual_address_page_base,
		};

		if (!request.IsValid()) return false;
		*out_request = request;
		return true;
	}

	bool BuildAndPublishRequestToCurrentOnlineTargets(
		RequestType request_type,
		std::uint16_t address_space_id,
		std::uintptr_t virtual_address_page_base,
		PublishedRequest* out_published_request) {
		if (!out_published_request) return false;

		const CpuMask sampled_target_cpu_mask = GetOnlineCpuMask();

		Request request{};
		switch (request_type) {
			case RequestType::InvalidateAsid:
				if (!PublishInvalidateAsidRequestToTargets(sampled_target_cpu_mask, address_space_id, &request)) return false;
				break;
			case RequestType::InvalidatePage:
				if (!PublishInvalidatePageRequestToTargets(
					sampled_target_cpu_mask,
					address_space_id,
					virtual_address_page_base,
					&request)) return false;
				break;
			case RequestType::InvalidateGlobalAll:
				if (!PublishInvalidateGlobalAllRequestToTargets(sampled_target_cpu_mask, &request)) return false;
				break;
			default:
				return false;
		}

		*out_published_request = PublishedRequest{
			.target_cpu_mask = sampled_target_cpu_mask,
			.request = request,
		};
		return true;
	}

	volatile std::uint64_t m_next_generation = 0;
	volatile std::uint64_t m_online_cpu_mask_bits = 0;
	volatile std::uint64_t m_online_cpu_mask_is_frozen = 0;
	Mailbox m_mailbox_by_core[CpuMask::kMaxCpuCount] = {};
	volatile std::uint64_t m_ack_generation_by_core[CpuMask::kMaxCpuCount] = {};

	public:
		static constexpr std::uint64_t kNoGeneration = 0;

		State() = default;

		void Reset() {
			Rocinante::AtomicStoreU64Db(&m_next_generation, kNoGeneration);
			Rocinante::AtomicStoreU64Db(&m_online_cpu_mask_bits, 0);
			Rocinante::AtomicStoreU64Db(&m_online_cpu_mask_is_frozen, 0);
			for (std::size_t core_index = 0; core_index < CpuMask::kMaxCpuCount; core_index++) {
				Rocinante::AtomicStoreU64Db(
					&m_mailbox_by_core[core_index].request_type,
					static_cast<std::uint64_t>(RequestType::None));
				Rocinante::AtomicStoreU64Db(&m_mailbox_by_core[core_index].address_space_id, 0);
				Rocinante::AtomicStoreU64Db(&m_mailbox_by_core[core_index].virtual_address_page_base, 0);
				Rocinante::AtomicStoreU64Db(&m_mailbox_by_core[core_index].published_generation, 0);
				Rocinante::AtomicStoreU64Db(&m_ack_generation_by_core[core_index], kNoGeneration);
			}
		}

		bool SetCpuOnline(std::uint32_t core_id, bool is_online) {
			if (!CpuMask::IsRepresentableCoreId(core_id)) return false;
			if (IsOnlineCpuMaskFrozen()) return false;

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

		bool FreezeOnlineCpuMask() {
			if (IsOnlineCpuMaskFrozen()) return true;
			Rocinante::AtomicStoreU64Db(&m_online_cpu_mask_is_frozen, 1);
			return true;
		}

		bool IsOnlineCpuMaskFrozen() const {
			return Rocinante::AtomicLoadU64AcqRel(&m_online_cpu_mask_is_frozen) != 0;
		}

		std::uint64_t AllocateGeneration() {
			return Rocinante::AtomicFetchAddU64Db(&m_next_generation, 1) + 1;
		}

		// Shooter-side helper: allocate a fresh generation and publish the
		// request to every target mailbox. The returned request object is the
		// token the shooter can later wait on via HaveAllTargetsAcknowledged().
		bool PublishInvalidateAsidRequestToTargets(
			CpuMask target_cpu_mask,
			std::uint16_t address_space_id,
			Request* out_request) {
			if (!AreTargetMailboxesAvailable(target_cpu_mask)) return false;

			Request request{};
			if (!BuildRequest(RequestType::InvalidateAsid, address_space_id, 0, &request)) return false;
			if (!PublishRequestToTargets(target_cpu_mask, request)) return false;
			if (out_request) *out_request = request;
			return true;
		}

		bool PublishInvalidatePageRequestToTargets(
			CpuMask target_cpu_mask,
			std::uint16_t address_space_id,
			std::uintptr_t virtual_address_page_base,
			Request* out_request) {
			if (!AreTargetMailboxesAvailable(target_cpu_mask)) return false;

			Request request{};
			if (!BuildRequest(RequestType::InvalidatePage, address_space_id, virtual_address_page_base, &request)) return false;
			if (!PublishRequestToTargets(target_cpu_mask, request)) return false;
			if (out_request) *out_request = request;
			return true;
		}

		bool PublishInvalidateGlobalAllRequestToTargets(CpuMask target_cpu_mask, Request* out_request) {
			if (!AreTargetMailboxesAvailable(target_cpu_mask)) return false;

			Request request{};
			if (!BuildRequest(RequestType::InvalidateGlobalAll, 0, 0, &request)) return false;
			if (!PublishRequestToTargets(target_cpu_mask, request)) return false;
			if (out_request) *out_request = request;
			return true;
		}

		// Shooter-side helper: sample the live online CPU mask once and bind that
		// snapshot to the newly published request generation.
		bool PublishInvalidateAsidRequestToCurrentOnlineTargets(
			std::uint16_t address_space_id,
			PublishedRequest* out_published_request) {
			return BuildAndPublishRequestToCurrentOnlineTargets(
				RequestType::InvalidateAsid,
				address_space_id,
				0,
				out_published_request);
		}

		bool PublishInvalidatePageRequestToCurrentOnlineTargets(
			std::uint16_t address_space_id,
			std::uintptr_t virtual_address_page_base,
			PublishedRequest* out_published_request) {
			return BuildAndPublishRequestToCurrentOnlineTargets(
				RequestType::InvalidatePage,
				address_space_id,
				virtual_address_page_base,
				out_published_request);
		}

		bool PublishInvalidateGlobalAllRequestToCurrentOnlineTargets(PublishedRequest* out_published_request) {
			return BuildAndPublishRequestToCurrentOnlineTargets(
				RequestType::InvalidateGlobalAll,
				0,
				0,
				out_published_request);
		}

		bool PublishRequestToCore(std::uint32_t core_id, const Request& request) {
			if (!CpuMask::IsRepresentableCoreId(core_id)) return false;
			if (!request.IsValid()) return false;

			if (!IsMailboxAvailableForCore(core_id)) return false;

			Mailbox& mailbox = m_mailbox_by_core[core_id];
			Rocinante::AtomicStoreU64Db(&mailbox.request_type, static_cast<std::uint64_t>(request.type));
			Rocinante::AtomicStoreU64Db(&mailbox.address_space_id, request.address_space_id);
			Rocinante::AtomicStoreU64Db(
				&mailbox.virtual_address_page_base,
				static_cast<std::uint64_t>(request.virtual_address_page_base));

			// Publication point for the mailbox contents: after this `_Db` store is
			// visible, the preceding request fields are also visible, and any future
			// transport layer must treat this as "publish before notify".
			Rocinante::AtomicStoreU64Db(&mailbox.published_generation, request.generation);
			return true;
		}

		bool PublishRequestToTargets(CpuMask target_cpu_mask, const Request& request) {
			if (!request.IsValid()) return false;

			std::uint64_t remaining_mask = target_cpu_mask.bits;
			for (std::uint32_t core_id = 0; remaining_mask != 0; core_id++) {
				const std::uint64_t current_core_bit = (1ull << core_id);
				if ((remaining_mask & current_core_bit) == 0) continue;

				if (!PublishRequestToCore(core_id, request)) return false;
				remaining_mask &= ~current_core_bit;
			}

			return true;
		}

		bool TryReadPublishedRequestForCore(std::uint32_t core_id, Request* out_request) const {
			if (!out_request) return false;
			if (!CpuMask::IsRepresentableCoreId(core_id)) return false;

			const Mailbox& mailbox = m_mailbox_by_core[core_id];
			// Acquire on the publication token first so the request payload reads
			// below observe the stores that were ordered before publication.
			const std::uint64_t published_generation = Rocinante::AtomicLoadU64AcqRel(&mailbox.published_generation);
			if (published_generation == 0) {
				return false;
			}

			const Request request{
				.generation = published_generation,
				.type = DecodeRequestType(Rocinante::AtomicLoadU64AcqRel(&mailbox.request_type)),
				.address_space_id = static_cast<std::uint16_t>(Rocinante::AtomicLoadU64AcqRel(&mailbox.address_space_id)),
				.virtual_address_page_base = static_cast<std::uintptr_t>(
					Rocinante::AtomicLoadU64AcqRel(&mailbox.virtual_address_page_base)),
			};

			if (!request.IsValid()) {
				return false;
			}

			*out_request = request;
			return true;
		}

		bool TryReadPendingRequestForCore(std::uint32_t core_id, Request* out_request) const {
			if (!out_request) return false;
			if (!TryReadPublishedRequestForCore(core_id, out_request)) return false;

			return GetAcknowledgedGeneration(core_id) < out_request->generation;
		}

		// Shooter-side helper: test whether a previously published request has
		// completed on every target core in the sampled target set.
		bool IsRequestCompletedForTargets(CpuMask target_cpu_mask, const Request& request) const {
			if (!request.IsValid()) return false;
			return HaveAllTargetsAcknowledged(target_cpu_mask, request.generation);
		}

		bool IsPublishedRequestCompleted(const PublishedRequest& published_request) const {
			if (!published_request.IsValid()) return false;
			return IsRequestCompletedForTargets(
				published_request.target_cpu_mask,
				published_request.request);
		}

		// Target-side helper: read the current pending mailbox request, invoke a
		// caller-supplied handler, and publish the ack generation only if the
		// handler reports success.
 		//
		// Ordering rule:
		// - The handler is where a future target CPU will perform the local
		//   `INVTLB` work.
		// - The final ack store is a `_Db` store, so the target's local work is
		//   ordered before other CPUs observe the acknowledgement.
		bool HandleAndAcknowledgePendingRequestForCore(
			std::uint32_t core_id,
			RequestHandler handler,
			void* context) {
			if (!handler) return false;

			Request request{};
			if (!TryReadPendingRequestForCore(core_id, &request)) return false;
			if (!handler(request, context)) return false;

			RecordAcknowledgedGeneration(core_id, request.generation);
			return true;
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

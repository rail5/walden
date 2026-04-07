/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/intrusive_rb_tree.h>
#include <src/memory/paging.h>

namespace Rocinante::Memory {

class VirtualMemoryAreaSet;
class AnonymousVmObject;

/**
 * @brief A minimal virtual memory area (VMA) record.
 *
 * Semantics:
 * - The range is [virtual_base, virtual_limit).
 * - The range is 4 KiB page aligned.
 */
struct VirtualMemoryArea final {
	enum class BackingType : std::uint8_t {
		Anonymous = 0,
	};

	std::uintptr_t virtual_base = 0;
	std::uintptr_t virtual_limit = 0;
	Paging::PagePermissions permissions{};
	BackingType backing_type = BackingType::Anonymous;
	// Backing object for anonymous mappings.
	//
	// Bring-up notes / flaws:
	// - This is currently optional so that a VMA can represent a reserved range
	//   before policy decides whether it should be demand-mapped.
	// - The VMM pager (Phase 4.3) requires this to be non-null to handle anonymous
	//   page faults.
	AnonymousVmObject* anonymous_object = nullptr;
	bool owns_frames = false;

	bool IsValid() const {
		if (virtual_base == 0) return false;
		if (virtual_limit == 0) return false;
		if (virtual_limit <= virtual_base) return false;
		if ((virtual_base % Paging::kPageSizeBytes) != 0) return false;
		if ((virtual_limit % Paging::kPageSizeBytes) != 0) return false;
		return true;
	}

	bool Contains(std::uintptr_t virtual_address) const {
		return (virtual_address >= virtual_base) && (virtual_address < virtual_limit);
	}

private:
	using RbLinks = Helpers::IntrusiveRedBlackTreeLinks<VirtualMemoryArea>;
	RbLinks m_rb{};

	friend class ::Rocinante::Helpers::IntrusiveRedBlackTree<
		VirtualMemoryArea,
		&VirtualMemoryArea::m_rb,
		&VirtualMemoryArea::virtual_base>;

	friend class VirtualMemoryAreaSet;
};

/**
 * @brief A set of non-overlapping VMAs stored in an intrusive balanced BST.
 *
 * This is an intrusive red-black tree keyed by `VirtualMemoryArea::virtual_base`.
 *
 * Rationale:
 * - We need a structure that scales to large numbers of VMAs without dynamic
 *   allocation inside the container.
 * - Using an intrusive tree keeps memory management explicit: the process/VMM
 *   layer owns VMA storage; the set only links nodes.
 *
 * Constraints / flaws (made explicit):
 * - There is no removal API yet.
 * - VMAs must have stable addresses while inserted (do not move/copy them).
 * - A VMA may be inserted into at most one set at a time.
 *
 * Invariants:
 * - VMAs are ordered by `virtual_base`.
 * - VMAs do not overlap.
 * - Tree links are owned by this set; callers must not modify them.
 */
class VirtualMemoryAreaSet final
	: private Helpers::IntrusiveRedBlackTree<
		VirtualMemoryArea,
		&VirtualMemoryArea::m_rb,
		&VirtualMemoryArea::virtual_base> {
public:
	VirtualMemoryAreaSet() = default;
	~VirtualMemoryAreaSet() = default;
	VirtualMemoryAreaSet(const VirtualMemoryAreaSet&) = delete;
	VirtualMemoryAreaSet& operator=(const VirtualMemoryAreaSet&) = delete;
	VirtualMemoryAreaSet(VirtualMemoryAreaSet&&) = delete;
	VirtualMemoryAreaSet& operator=(VirtualMemoryAreaSet&&) = delete;

	std::size_t AreaCount() const { return NodeCount(); }

	const VirtualMemoryArea* FindVmaForAddress(std::uintptr_t virtual_address) const {
		const auto* candidate = FindPredecessorOrEqual(virtual_address);
		if (!candidate) return nullptr;
		if (!candidate->Contains(virtual_address)) return nullptr;
		return candidate;
	}

	bool Insert(VirtualMemoryArea* area) {
		using Tree = Helpers::IntrusiveRedBlackTree<
			VirtualMemoryArea,
			&VirtualMemoryArea::m_rb,
			&VirtualMemoryArea::virtual_base>;

		if (!area) return false;
		if (!area->IsValid()) return false;

		// Non-overlap is enforced by checking only adjacent nodes in the ordering.
		//
		// Note: using predecessor/successor queries keeps policy (non-overlap)
		// outside the generic tree implementation.
		const auto* predecessor = FindPredecessorOrEqual(area->virtual_base);
		const auto* successor = FindSuccessorOrEqual(area->virtual_base);
		if (predecessor && area->virtual_base < predecessor->virtual_limit) return false;
		if (successor && area->virtual_limit > successor->virtual_base) return false;

		return Tree::Insert(area);
	}

	#if defined(ROCINANTE_TESTS)
	bool DebugValidateInvariants() const {
		if (!DebugValidateRbInvariants()) return false;

		std::size_t visited_count = 0;
		const VirtualMemoryArea* previous = nullptr;
		for (const VirtualMemoryArea* node = First(); node; node = Next(node)) {
			visited_count++;
			if (previous && node->virtual_base < previous->virtual_limit) return false;
			previous = node;
		}

		return visited_count == NodeCount();
	}
	#endif
};

} // namespace Rocinante::Memory

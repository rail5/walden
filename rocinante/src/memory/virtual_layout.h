/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante::Memory {

/**
 * @brief Kernel virtual address layout policy (bring-up version).
 *
 * This file is intentionally small and explicit: it captures the *policy*
 * choices we are making for virtual addresses.
 *
 * Flaw / bring-up note:
 * - This file defines a VALEN-derived higher-half base (canonical/sign-extended)
 *   plus small policy offsets.
 * - The higher-half / physmap strategy is still under active bring-up; the
 *   chosen offsets must be validated against the final virtual memory map.
 */
namespace VirtualLayout {

	// Canonical/sign-extended address policy:
	// For an implemented virtual address width VALEN=N (LA64), the CPU expects
	// bits [63:N] to be a sign-extension of bit [N-1]. The smallest canonical
	// address in the higher half is therefore:
	//   base = (~0 << N) | (1 << (N-1))
	static constexpr std::uintptr_t CanonicalHighHalfBase(std::uint8_t virtual_address_bits) {
		if (virtual_address_bits == 0) return 0;
		if (virtual_address_bits >= 64) return 0;
		return (static_cast<std::uintptr_t>(~0ull) << virtual_address_bits) |
			(static_cast<std::uintptr_t>(1ull) << (virtual_address_bits - 1));
	}

	// Policy offsets within the higher-half region.
	//
	// Bring-up rationale:
	// Keep the physmap close to the higher-half base but separate from the kernel
	// image mapping. Using a small offset keeps this representable for smaller
	// (but still practical) VALEN values.
	static constexpr std::uintptr_t kKernelHigherHalfOffsetBytes = 0;
	static constexpr std::uintptr_t kPhysMapOffsetBytes = 0x40000000ull; // 1 GiB

	static constexpr std::uintptr_t KernelHigherHalfBase(std::uint8_t virtual_address_bits) {
		return CanonicalHighHalfBase(virtual_address_bits) + kKernelHigherHalfOffsetBytes;
	}

	static constexpr std::uintptr_t PhysMapBase(std::uint8_t virtual_address_bits) {
		return CanonicalHighHalfBase(virtual_address_bits) + kPhysMapOffsetBytes;
	}

	static constexpr std::uintptr_t ToPhysMapVirtual(std::uintptr_t physical_address, std::uint8_t virtual_address_bits) {
		return PhysMapBase(virtual_address_bits) + physical_address;
	}

	static constexpr std::uintptr_t FromPhysMapVirtual(std::uintptr_t virtual_address, std::uint8_t virtual_address_bits) {
		return virtual_address - PhysMapBase(virtual_address_bits);
	}

} // namespace VirtualLayout

} // namespace Rocinante::Memory

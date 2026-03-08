/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

namespace Rocinante {

/**
 * @brief Read the current processor core ID.
 *
 * This is the architected per-core identity value exposed by `CSR.CPUID.CoreID`.
 *
 * Spec anchor (LoongArch-Vol1-EN.html):
 * - CSR list: `CPUID` is CSR number 0x20.
 * - Section 7.4.12 (CPU Identity / CPUID):
 *   - `CoreID` is bits [8:0].
 *   - It is used by software to distinguish individual processor cores in a multi-core system.
 */
static inline std::uint16_t ReadCurrentProcessorCoreId() {
	// CSR.CPUID (CPU Identity)
	static constexpr std::uint32_t kCsrCpuIdentity = 0x20;

	// CPUID.CoreID is bits [8:0].
	static constexpr std::uint64_t kCoreIdMask = 0x1FFull;

	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(kCsrCpuIdentity));
	return static_cast<std::uint16_t>(value & kCoreIdMask);
}

} // namespace Rocinante

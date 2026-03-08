/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include <src/sp/cpucfg.h>

namespace Rocinante {

namespace Detail {

// LoongArch AM* atomic access instructions.
//
// Spec anchor (LoongArch-Vol1-EN.html):
// - Section 2.2.7: AM* atomic access instructions perform an atomic
//   "read-modify-write" sequence.
// - `AM*_DB.{W/D}` additionally implements a data barrier function:
//   access operations before are completed before execution; access operations
//   after are allowed only after completion.

static inline std::uint64_t AtomicFetchAddU64DbViaLlSc(volatile std::uint64_t* address, std::uint64_t addend) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.4 (LL.{W/D}, SC.{W/D}): used to implement an atomic
	//   "read, modify, and write" sequence by looping until SC succeeds.
	// - Section 2.2.8.1 (DBAR 0): full load/store barrier.
	//
	// Note: LL/SC requires a cache-consistent (CC) memory attribute for the
	// accessed address; otherwise results are indeterminate (per spec).
	asm volatile("dbar 0" ::: "memory");

	std::uint64_t old_value;
	std::uint64_t sc_result;
	asm volatile(
		"1:\n"
		"ll.d %0, %2, 0\n"
		"add.d %1, %0, %3\n"
		"sc.d %1, %2, 0\n"
		"beqz %1, 1b\n"
		: "=&r"(old_value), "=&r"(sc_result)
		: "r"(address), "r"(addend)
		: "memory"
	);

	asm volatile("dbar 0" ::: "memory");
	return old_value;
}

static inline std::uint64_t AtomicFetchAddU64DbViaAmaddDb(volatile std::uint64_t* address, std::uint64_t addend) {
	std::uint64_t old_value;
	asm volatile(
		"amadd_db.d %0, %1, %2"
		: "=&r"(old_value)
		: "r"(addend), "r"(address)
		: "memory"
	);
	return old_value;
}

} // namespace Detail

static inline std::uint64_t AtomicFetchAddU64Db(volatile std::uint64_t* address, std::uint64_t addend) {
	// Prefer AM* if present; otherwise fall back to LL/SC.
	//
	// CPUCFG bit (LoongArch-Vol1-EN.html):
	// - CPUCFG word 0x2 bit 22 (LAM): 1 indicates support for AM* atomic
	//   memory access instructions.
	if (Rocinante::GetCPUCFG().SupportsAMAtomicMemoryAccess()) {
		return Rocinante::Detail::AtomicFetchAddU64DbViaAmaddDb(address, addend);
	}

	return Rocinante::Detail::AtomicFetchAddU64DbViaLlSc(address, addend);
}

} // namespace Rocinante

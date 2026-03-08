/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include <src/sp/cpucfg.h>

namespace Rocinante {

namespace Detail {

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

static inline std::uint64_t AtomicFetchAddU64AcqRelViaLlAcqScRel(volatile std::uint64_t* address, std::uint64_t addend) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.6 (LL.ACQ.{W/D}, SC.REL.{W/D}): LL.ACQ has read-acquire
	//   semantics; SC.REL has write-release semantics; used in a retry loop
	//   until SC succeeds.
	//
	// Note: LL/SC requires a cache-consistent (CC) memory attribute for the
	// accessed address; otherwise results are indeterminate (per spec).
	std::uint64_t old_value;
	std::uint64_t sc_result;
	asm volatile(
		"1:\n"
		"llacq.d %0, %2\n"
		"add.d %1, %0, %3\n"
		"screl.d %1, %2\n"
		"beqz %1, 1b\n"
		: "=&r"(old_value), "=&r"(sc_result)
		: "r"(address), "r"(addend)
		: "memory"
	);

	return old_value;
}

// LoongArch AM* atomic access instructions.

// Spec anchor (LoongArch-Vol1-EN.html):
// - Section 2.2.7: AM* atomic access instructions perform an atomic
//   "read-modify-write" sequence.
// - `AM*_DB.{W/D}` additionally implements a data barrier function:
//   access operations before are completed before execution; access operations
//   after are allowed only after completion.

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

static inline std::uint64_t AtomicExchangeU64DbViaLlSc(volatile std::uint64_t* address, std::uint64_t desired) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.4 (LL.{W/D}, SC.{W/D}): used to implement an atomic
	//   "read, modify, and write" sequence by looping until SC succeeds.
	// - Section 2.2.8.1 (DBAR 0): full load/store barrier.
	asm volatile("dbar 0" ::: "memory");

	std::uint64_t old_value;
	std::uint64_t sc_result;
	asm volatile(
		"1:\n"
		"ll.d %0, %2, 0\n"
		"or %1, %3, $zero\n"
		"sc.d %1, %2, 0\n"
		"beqz %1, 1b\n"
		: "=&r"(old_value), "=&r"(sc_result)
		: "r"(address), "r"(desired)
		: "memory"
	);

	asm volatile("dbar 0" ::: "memory");
	return old_value;
}

static inline std::uint64_t AtomicExchangeU64DbViaAmswapDb(volatile std::uint64_t* address, std::uint64_t desired) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.1: AMSWAP[.DB].D returns the old value and writes the
	//   new value from the general register.
	// - Section 2.2.7.1: the _DB form provides a data barrier function.
	std::uint64_t old_value;
	asm volatile(
		"amswap_db.d %0, %1, %2"
		: "=&r"(old_value)
		: "r"(desired), "r"(address)
		: "memory"
	);
	return old_value;
}

static inline bool AtomicCompareExchangeU64DbViaLlSc(
	volatile std::uint64_t* address,
	std::uint64_t* expected_in_out,
	std::uint64_t desired) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.4 (LL.{W/D}, SC.{W/D}): implement atomic RMW with a retry loop.
	// - Section 2.2.8.1 (DBAR 0): full load/store barrier.
	asm volatile("dbar 0" ::: "memory");

	const std::uint64_t expected = *expected_in_out;

	for (;;) {
		std::uint64_t old_value;
		asm volatile(
			"ll.d %0, %1, 0\n"
			: "=&r"(old_value)
			: "r"(address)
			: "memory"
		);

		if (old_value != expected) {
			*expected_in_out = old_value;
			asm volatile("dbar 0" ::: "memory");
			return false;
		}

		std::uint64_t sc_result = desired;
		asm volatile(
			"sc.d %0, %1, 0\n"
			: "+&r"(sc_result)
			: "r"(address)
			: "memory"
		);

		if (sc_result != 0) {
			*expected_in_out = old_value;
			asm volatile("dbar 0" ::: "memory");
			return true;
		}
		// SC failed (lost LLbit). Retry.
	}
}

static inline bool AtomicCompareExchangeU64DbViaAmcasDb(
	volatile std::uint64_t* address,
	std::uint64_t* expected_in_out,
	std::uint64_t desired) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.3 (AMCAS[_DB].D): compare old memory value against
	//   expected (in rd); write desired (rk) on match; rd receives old value.
	// - Section 2.2.7.3: AMCAS_DB provides a data barrier function.
	//
	// Spec constraint: avoid using the same register number for rd and rk.
	// We enforce this by pinning them to distinct temporaries.
	const std::uint64_t expected = *expected_in_out;

	register std::uint64_t rd asm("$t1") = expected;
	register std::uint64_t rk asm("$t0") = desired;
	asm volatile(
		"amcas_db.d %0, %1, %2"
		: "+r"(rd)
		: "r"(rk), "r"(address)
		: "memory"
	);

	*expected_in_out = rd;
	return (rd == expected);
}

} // namespace Detail

// Higher-level atomic operations that select the best implementation based on CPU features.

// AtomicFetchAddU64Db performs an atomic fetch-and-add on a 64-bit value
// with a data barrier, using AM* instructions if supported or falling back to LL/SC otherwise.
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

// AtomicFetchAddU64AcqRel performs an atomic fetch-and-add on a 64-bit value
// with acquire/release semantics.
//
// If LL.ACQ/SC.REL is supported, this uses that pair; otherwise it falls back
// to the stronger AtomicFetchAddU64Db (full barrier) implementation.
static inline std::uint64_t AtomicFetchAddU64AcqRel(volatile std::uint64_t* address, std::uint64_t addend) {
	// CPUCFG bit (LoongArch-Vol1-EN.html):
	// - CPUCFG word 0x2 bit 29 (LLACQ_SCREL): 1 indicates support for
	//   LL.ACQ.{W/D} and SC.REL.{W/D}.
	if (Rocinante::GetCPUCFG().SupportsLLACQSCREL()) {
		return Rocinante::Detail::AtomicFetchAddU64AcqRelViaLlAcqScRel(address, addend);
	}

	return Rocinante::AtomicFetchAddU64Db(address, addend);
}

// AtomicExchangeU64Db performs an atomic exchange on a 64-bit value with a
// full load/store barrier.
static inline std::uint64_t AtomicExchangeU64Db(volatile std::uint64_t* address, std::uint64_t desired) {
	if (Rocinante::GetCPUCFG().SupportsAMAtomicMemoryAccess()) {
		return Rocinante::Detail::AtomicExchangeU64DbViaAmswapDb(address, desired);
	}

	return Rocinante::Detail::AtomicExchangeU64DbViaLlSc(address, desired);
}

// AtomicCompareExchangeU64Db atomically compares *address with *expected.
// If equal, it writes desired into *address. Returns true on success.
// On failure, *expected is updated with the observed value.
static inline bool AtomicCompareExchangeU64Db(
	volatile std::uint64_t* address,
	std::uint64_t* expected_in_out,
	std::uint64_t desired) {
	// CPUCFG bit (LoongArch-Vol1-EN.html):
	// - CPUCFG word 0x2 bit 28 (LAMCAS): 1 indicates support for
	//   AMCAS[_DB].{B/H/W/D}.
	if (Rocinante::GetCPUCFG().SupportsLAMCAS()) {
		return Rocinante::Detail::AtomicCompareExchangeU64DbViaAmcasDb(address, expected_in_out, desired);
	}

	return Rocinante::Detail::AtomicCompareExchangeU64DbViaLlSc(address, expected_in_out, desired);
}

} // namespace Rocinante

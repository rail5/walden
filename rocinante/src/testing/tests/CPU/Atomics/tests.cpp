/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/atomic.h>

#include <cstdint>

namespace Rocinante::Testing {

namespace {

static void Test_Atomics_FetchAddU64Db_BasicSemantics(TestContext* ctx) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7: AM* atomically performs a "read-modify-write" sequence.
	// - Section 2.2.7.4: LL/SC pair can implement atomic RMW via retry loop.
	// - Section 2.2.7.1: `AM*_DB.{W/D}` provides a data barrier function.
	// - Section 2.2.8.1: `DBAR 0` provides a full load/store barrier (used by
	//   the LL/SC fallback to preserve the "Db" intent).

	alignas(8) volatile std::uint64_t value = 41;

	if (Rocinante::GetCPUCFG().SupportsAMAtomicMemoryAccess()) {
		// Output a note that we'll be starting by testing the AM* instruction path
		Rocinante::Testing::Note(ctx, __FILE__, __LINE__, "Testing AM* instruction path");
	} else {
		// Output a note that we'll be *unable* to test the AM* instruction path, but we'll still test the LL/SC fallback.
		Rocinante::Testing::Note(ctx, __FILE__, __LINE__, "CPU does not support AM* atomic memory access instructions; testing LL/SC fallback only");
	}

	const std::uint64_t old0 = Rocinante::AtomicFetchAddU64Db(&value, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, old0, 41);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 42);

	const std::uint64_t old1 = Rocinante::AtomicFetchAddU64Db(&value, 5);
	ROCINANTE_EXPECT_EQ_U64(ctx, old1, 42);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 47);

	const std::uint64_t old2 = Rocinante::AtomicFetchAddU64Db(&value, 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, old2, 47);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 47);

	if (Rocinante::GetCPUCFG().SupportsAMAtomicMemoryAccess()) {
		// Assumption: If the CPU supports AM* atomic access instructions,
		// then the above results used them.
		// Now, we should also verify that the fallback path (LL/SC) produces the same results.

		// Output a note that we'll be testing the LL/SC fallback path
		Rocinante::Testing::Note(ctx, __FILE__, __LINE__, "Testing LL/SC fallback path");

		alignas(8) volatile std::uint64_t llsc_value = 100;
		const std::uint64_t old0 = Rocinante::Detail::AtomicFetchAddU64DbViaLlSc(&llsc_value, 10);
		ROCINANTE_EXPECT_EQ_U64(ctx, old0, 100);
		ROCINANTE_EXPECT_EQ_U64(ctx, llsc_value, 110);

		const std::uint64_t old1 = Rocinante::Detail::AtomicFetchAddU64DbViaLlSc(&llsc_value, 20);
		ROCINANTE_EXPECT_EQ_U64(ctx, old1, 110);
		ROCINANTE_EXPECT_EQ_U64(ctx, llsc_value, 130);

		const std::uint64_t old2 = Rocinante::Detail::AtomicFetchAddU64DbViaLlSc(&llsc_value, 0);
		ROCINANTE_EXPECT_EQ_U64(ctx, old2, 130);
		ROCINANTE_EXPECT_EQ_U64(ctx, llsc_value, 130);
	}
}

static void Test_Atomics_FetchAddU64AcqRel_BasicSemantics(TestContext* ctx) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.6: LL.ACQ has read-acquire semantics; SC.REL has
	//   write-release semantics; used as an atomic RMW pair.
	alignas(8) volatile std::uint64_t value = 7;

	const std::uint64_t old0 = Rocinante::AtomicFetchAddU64AcqRel(&value, 3);
	ROCINANTE_EXPECT_EQ_U64(ctx, old0, 7);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 10);

	const std::uint64_t old1 = Rocinante::AtomicFetchAddU64AcqRel(&value, 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, old1, 10);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 10);

	if (Rocinante::GetCPUCFG().SupportsLLACQSCREL()) {
		Rocinante::Testing::Note(ctx, __FILE__, __LINE__, "Testing LL.ACQ/SC.REL helper path");

		alignas(8) volatile std::uint64_t llacq_value = 1000;
		const std::uint64_t old2 = Rocinante::Detail::AtomicFetchAddU64AcqRelViaLlAcqScRel(&llacq_value, 24);
		ROCINANTE_EXPECT_EQ_U64(ctx, old2, 1000);
		ROCINANTE_EXPECT_EQ_U64(ctx, llacq_value, 1024);
	}
}

static void Test_Atomics_ExchangeU64Db_BasicSemantics(TestContext* ctx) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.1: AMSWAP[.DB].D atomically swaps memory with a register.
	// - Section 2.2.7.4: LL/SC pair can implement atomic RMW via retry loop.
	alignas(8) volatile std::uint64_t value = 11;

	const std::uint64_t old0 = Rocinante::AtomicExchangeU64Db(&value, 99);
	ROCINANTE_EXPECT_EQ_U64(ctx, old0, 11);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 99);

	const std::uint64_t old1 = Rocinante::AtomicExchangeU64Db(&value, 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, old1, 99);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 0);

	if (Rocinante::GetCPUCFG().SupportsAMAtomicMemoryAccess()) {
		Rocinante::Testing::Note(ctx, __FILE__, __LINE__, "Testing LL/SC exchange helper path");

		alignas(8) volatile std::uint64_t llsc_value = 100;
		const std::uint64_t old2 = Rocinante::Detail::AtomicExchangeU64DbViaLlSc(&llsc_value, 1234);
		ROCINANTE_EXPECT_EQ_U64(ctx, old2, 100);
		ROCINANTE_EXPECT_EQ_U64(ctx, llsc_value, 1234);
	}
}

static void Test_Atomics_CompareExchangeU64Db_BasicSemantics(TestContext* ctx) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.3: AMCAS compares old memory value against expected and
	//   conditionally writes new value; rd receives old value.
	// - Section 2.2.7.4: LL/SC pair can implement the same semantics via retry loop.
	alignas(8) volatile std::uint64_t value = 5;

	std::uint64_t expected0 = 5;
	const bool ok0 = Rocinante::AtomicCompareExchangeU64Db(&value, &expected0, 9);
	ROCINANTE_EXPECT_TRUE(ctx, ok0);
	ROCINANTE_EXPECT_EQ_U64(ctx, expected0, 5);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 9);

	std::uint64_t expected1 = 5;
	const bool ok1 = Rocinante::AtomicCompareExchangeU64Db(&value, &expected1, 77);
	ROCINANTE_EXPECT_TRUE(ctx, !ok1);
	ROCINANTE_EXPECT_EQ_U64(ctx, expected1, 9);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 9);

	if (Rocinante::GetCPUCFG().SupportsLAMCAS()) {
		Rocinante::Testing::Note(ctx, __FILE__, __LINE__, "Testing LL/SC compare-exchange helper path");

		alignas(8) volatile std::uint64_t llsc_value = 123;
		std::uint64_t expected2 = 123;
		const bool ok2 = Rocinante::Detail::AtomicCompareExchangeU64DbViaLlSc(&llsc_value, &expected2, 321);
		ROCINANTE_EXPECT_TRUE(ctx, ok2);
		ROCINANTE_EXPECT_EQ_U64(ctx, expected2, 123);
		ROCINANTE_EXPECT_EQ_U64(ctx, llsc_value, 321);
	}
}

static void Test_Atomics_LoadStoreWrappers_BasicSemantics(TestContext* ctx) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 2.2.7.6: LL.ACQ/SC.REL gives read-acquire / write-release
	//   semantics for the RMW primitive that backs AtomicLoadU64AcqRel.
	// - Section 2.2.7.1: AMSWAP_DB.D provides an atomic exchange with a data
	//   barrier, which backs AtomicStoreU64Db.
	// - Section 2.2.8.1: DBAR 0 is a full load/store barrier, used by the
	//   stronger fallback paths.
	alignas(8) volatile std::uint64_t value = 55;

	const std::uint64_t loaded0 = Rocinante::AtomicLoadU64AcqRel(&value);
	ROCINANTE_EXPECT_EQ_U64(ctx, loaded0, 55);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 55);

	Rocinante::AtomicStoreU64Db(&value, 77);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 77);

	const std::uint64_t loaded1 = Rocinante::AtomicLoadU64AcqRel(&value);
	ROCINANTE_EXPECT_EQ_U64(ctx, loaded1, 77);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 77);
	Rocinante::AtomicStoreU64Db(&value, 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, value, 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, Rocinante::AtomicLoadU64AcqRel(&value), 0);
}

} // namespace

void TestEntry_Atomics_FetchAddU64Db_BasicSemantics(TestContext* ctx) {
	Test_Atomics_FetchAddU64Db_BasicSemantics(ctx);
}

void TestEntry_Atomics_FetchAddU64AcqRel_BasicSemantics(TestContext* ctx) {
	Test_Atomics_FetchAddU64AcqRel_BasicSemantics(ctx);
}

void TestEntry_Atomics_ExchangeU64Db_BasicSemantics(TestContext* ctx) {
	Test_Atomics_ExchangeU64Db_BasicSemantics(ctx);
}

void TestEntry_Atomics_CompareExchangeU64Db_BasicSemantics(TestContext* ctx) {
	Test_Atomics_CompareExchangeU64Db_BasicSemantics(ctx);
}

void TestEntry_Atomics_LoadStoreWrappers_BasicSemantics(TestContext* ctx) {
	Test_Atomics_LoadStoreWrappers_BasicSemantics(ctx);
}

} // namespace Rocinante::Testing

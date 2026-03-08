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

} // namespace

void TestEntry_Atomics_FetchAddU64Db_BasicSemantics(TestContext* ctx) {
	Test_Atomics_FetchAddU64Db_BasicSemantics(ctx);
}

} // namespace Rocinante::Testing

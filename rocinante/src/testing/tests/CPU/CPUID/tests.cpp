/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpuid.h>

#include <cstdint>

namespace Rocinante::Testing {

namespace {

static void Test_CPUID_CoreId_IsReadableAndStable(TestContext* ctx) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 7.4.12 (CPU Identity / CPUID): CoreID is bits [8:0].
	const std::uint16_t core_id_a = Rocinante::ReadCurrentProcessorCoreId();
	const std::uint16_t core_id_b = Rocinante::ReadCurrentProcessorCoreId();

	ROCINANTE_EXPECT_TRUE(ctx, core_id_a <= 0x1FFu);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(core_id_a), static_cast<std::uint64_t>(core_id_b));
}

} // namespace

void TestEntry_CPUID_CoreId_IsReadableAndStable(TestContext* ctx) {
	Test_CPUID_CoreId_IsReadableAndStable(ctx);
}

} // namespace Rocinante::Testing

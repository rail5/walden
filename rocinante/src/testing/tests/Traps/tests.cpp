/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <cstdint>

namespace Rocinante::Testing {

namespace {

static void Test_Traps_BREAK_EntersAndReturns(TestContext* ctx) {
	ResetTrapObservations();

	// The BREAK instruction causes a synchronous exception.
	//
	// This test proves:
	// - the trap entry path is wired up,
	// - the handler can observe EXCCODE=BREAK, and
	// - the ERTN (Exception Return) path works after we adjust the saved
	//   exception return address.
	asm volatile("break 0" ::: "memory");

	ROCINANTE_EXPECT_EQ_U64(ctx, BreakTrapCount(), 1);
}

static void Test_Traps_INE_UndefinedInstruction_IsObserved(TestContext* ctx) {
	ResetTrapObservations();

	// Table 21: EXCCODE 0xD => INE (Instruction Non-defined Exception).
	static constexpr std::uint64_t kExceptionCodeIne = 0xD;
	ArmExpectedTrap(kExceptionCodeIne);

	// Emit an instruction encoding that is not defined.
	asm volatile(".word 0xffffffff" ::: "memory");

	ROCINANTE_EXPECT_TRUE(ctx, ExpectedTrapObserved());
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapExceptionCode(), kExceptionCodeIne);
}

} // namespace

void TestEntry_Traps_BREAK_EntersAndReturns(TestContext* ctx) {
	Test_Traps_BREAK_EntersAndReturns(ctx);
}

void TestEntry_Traps_INE_UndefinedInstruction_IsObserved(TestContext* ctx) {
	Test_Traps_INE_UndefinedInstruction_IsObserved(ctx);
}

} // namespace Rocinante::Testing

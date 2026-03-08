/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/memory/tlb_shootdown_state.h>

namespace Rocinante::Testing {

namespace {

static void Test_TlbShootdown_CpuMask_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::CpuMask;

	CpuMask mask;
	ROCINANTE_EXPECT_TRUE(ctx, mask.IsEmpty());
	ROCINANTE_EXPECT_TRUE(ctx, mask.Add(0));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Add(5));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, !mask.Contains(1));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Contains(5));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Remove(0));
	ROCINANTE_EXPECT_TRUE(ctx, !mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, !mask.Add(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount)));

	const CpuMask single_core = CpuMask::ForCore(3);
	ROCINANTE_EXPECT_TRUE(ctx, single_core.Contains(3));
	ROCINANTE_EXPECT_EQ_U64(ctx, single_core.bits, (1ull << 3));
	ROCINANTE_EXPECT_TRUE(ctx, CpuMask::ForCore(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount)).IsEmpty());
}

static void Test_TlbShootdown_State_GenerationAck_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::CpuMask;
	using Rocinante::Memory::TlbShootdown::State;

	State state;
	state.Reset();

	ROCINANTE_EXPECT_TRUE(ctx, state.GetOnlineCpuMask().IsEmpty());
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(0, true));
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, true));
	ROCINANTE_EXPECT_TRUE(ctx, !state.SetCpuOnline(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount), true));

	const CpuMask online_mask = state.GetOnlineCpuMask();
	ROCINANTE_EXPECT_TRUE(ctx, online_mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, online_mask.Contains(2));
	ROCINANTE_EXPECT_TRUE(ctx, !online_mask.Contains(1));

	const std::uint64_t generation1 = state.AllocateGeneration();
	const std::uint64_t generation2 = state.AllocateGeneration();
	ROCINANTE_EXPECT_EQ_U64(ctx, generation1, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, generation2, 2);

	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(online_mask, generation2));

	state.RecordAcknowledgedGeneration(0, generation2);
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(online_mask, generation2));

	// A later acknowledged generation must also satisfy an earlier wait.
	state.RecordAcknowledgedGeneration(2, generation2 + 1);
	ROCINANTE_EXPECT_TRUE(ctx, state.HaveAllTargetsAcknowledged(online_mask, generation2));
	ROCINANTE_EXPECT_EQ_U64(ctx, state.GetAcknowledgedGeneration(2), generation2 + 1);

	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, false));
	const CpuMask reduced_online_mask = state.GetOnlineCpuMask();
	ROCINANTE_EXPECT_TRUE(ctx, reduced_online_mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, !reduced_online_mask.Contains(2));
	ROCINANTE_EXPECT_TRUE(ctx, state.HaveAllTargetsAcknowledged(reduced_online_mask, generation2));
	ROCINANTE_EXPECT_EQ_U64(ctx, state.GetAcknowledgedGeneration(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount)), 0);
}

} // namespace

void TestEntry_TlbShootdown_CpuMask_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_CpuMask_BasicSemantics(ctx);
}

void TestEntry_TlbShootdown_State_GenerationAck_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_State_GenerationAck_BasicSemantics(ctx);
}

} // namespace Rocinante::Testing

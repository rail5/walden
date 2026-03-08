/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/memory/vma.h>

namespace Rocinante::Testing {

namespace {

static void Test_VMM_VMA_InsertLookup(TestContext* ctx) {
	using Rocinante::Memory::VirtualMemoryArea;
	using Rocinante::Memory::VirtualMemoryAreaSet;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::PagePermissions;

	VirtualMemoryAreaSet set;
	ROCINANTE_EXPECT_EQ_U64(ctx, set.AreaCount(), 0);

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	VirtualMemoryArea a_mut;
	a_mut.virtual_base = 0x4000;
	a_mut.virtual_limit = 0x8000;
	a_mut.permissions = permissions;
	a_mut.backing_type = VirtualMemoryArea::BackingType::Anonymous;
	a_mut.owns_frames = true;

	VirtualMemoryArea b_mut;
	b_mut.virtual_base = 0x8000;
	b_mut.virtual_limit = 0xC000;
	b_mut.permissions = permissions;
	b_mut.backing_type = VirtualMemoryArea::BackingType::Anonymous;
	b_mut.owns_frames = true;

	ROCINANTE_EXPECT_TRUE(ctx, set.Insert(&a_mut));
	ROCINANTE_EXPECT_TRUE(ctx, set.Insert(&b_mut));
	ROCINANTE_EXPECT_EQ_U64(ctx, set.AreaCount(), 2);
	#if defined(ROCINANTE_TESTS) // In fact if we're running tests then ROCINANTE_TESTS is of course defined
	// But if we remove the 'if defined' guard, then our static analyzer starts complaining about the `DebugValidateInvariants()` method being undefined
	// Why? Because `DebugValidateInvariants()` is only defined in test builds (guarded by `#if defined(ROCINANTE_TESTS)` in its own definition)
	ROCINANTE_EXPECT_TRUE(ctx, set.DebugValidateInvariants());
	#endif

	// Lookup inside each VMA.
	{
		const auto* found = set.FindVmaForAddress(0x4000);
		ROCINANTE_EXPECT_TRUE(ctx, found != nullptr);
		if (found) {
			ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_base, 0x4000);
			ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_limit, 0x8000);
		}
	}
	{
		const auto* found = set.FindVmaForAddress(0x7FFF);
		ROCINANTE_EXPECT_TRUE(ctx, found != nullptr);
		if (found) {
			ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_base, 0x4000);
		}
	}
	{
		const auto* found = set.FindVmaForAddress(0x8000);
		ROCINANTE_EXPECT_TRUE(ctx, found != nullptr);
		if (found) {
			ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_base, 0x8000);
			ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_limit, 0xC000);
		}
	}

	// Lookup outside returns null.
	ROCINANTE_EXPECT_TRUE(ctx, set.FindVmaForAddress(0x0000) == nullptr);
	ROCINANTE_EXPECT_TRUE(ctx, set.FindVmaForAddress(0xC000) == nullptr);
	ROCINANTE_EXPECT_TRUE(ctx, set.FindVmaForAddress(0xFFFF) == nullptr);

	// Overlapping insertion is rejected.
	VirtualMemoryArea overlap_mut;
	overlap_mut.virtual_base = 0x7000;
	overlap_mut.virtual_limit = 0x9000;
	overlap_mut.permissions = permissions;
	overlap_mut.backing_type = VirtualMemoryArea::BackingType::Anonymous;
	overlap_mut.owns_frames = true;
	ROCINANTE_EXPECT_TRUE(ctx, !set.Insert(&overlap_mut));

	// Misaligned insertion is rejected.
	VirtualMemoryArea misaligned_mut;
	misaligned_mut.virtual_base = 0x1234;
	misaligned_mut.virtual_limit = 0x2000;
	misaligned_mut.permissions = permissions;
	misaligned_mut.backing_type = VirtualMemoryArea::BackingType::Anonymous;
	misaligned_mut.owns_frames = true;
	ROCINANTE_EXPECT_TRUE(ctx, !set.Insert(&misaligned_mut));

	// Stress insertion order to exercise balancing and invariant checks.
	{
		VirtualMemoryAreaSet many;
		static constexpr std::size_t kManyCount = 128;
		VirtualMemoryArea many_areas[kManyCount]{};
		for (std::size_t i = 0; i < kManyCount; i++) {
			const std::uintptr_t base = 0x100000 + (i * 0x4000);
			many_areas[i].virtual_base = base;
			many_areas[i].virtual_limit = base + 0x4000;
			many_areas[i].permissions = permissions;
			many_areas[i].backing_type = VirtualMemoryArea::BackingType::Anonymous;
			many_areas[i].owns_frames = true;
			ROCINANTE_EXPECT_TRUE(ctx, many.Insert(&many_areas[i]));
		}
		ROCINANTE_EXPECT_EQ_U64(ctx, many.AreaCount(), kManyCount);
		#if defined(ROCINANTE_TESTS) // See above comment about the `if defined` guard
		ROCINANTE_EXPECT_TRUE(ctx, many.DebugValidateInvariants());
		#endif
		for (std::size_t i = 0; i < kManyCount; i++) {
			const std::uintptr_t base = 0x100000 + (i * 0x4000);
			const auto* found = many.FindVmaForAddress(base);
			ROCINANTE_EXPECT_TRUE(ctx, found != nullptr);
			if (found) {
				ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_base, base);
				ROCINANTE_EXPECT_EQ_U64(ctx, found->virtual_limit, base + 0x4000);
			}
		}
	}
}

} // namespace

void TestEntry_VMM_VMA_InsertLookup(TestContext* ctx) {
	Test_VMM_VMA_InsertLookup(ctx);
}

} // namespace Rocinante::Testing

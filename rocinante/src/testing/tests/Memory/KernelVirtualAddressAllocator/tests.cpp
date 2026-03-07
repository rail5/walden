/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/memory/kernel_va_allocator.h>

namespace Rocinante::Testing {

namespace {

static void Test_KernelVirtualAddressAllocator_AllocateFreeCoalesce(TestContext* ctx) {
	using Rocinante::Memory::KernelVirtualAddressAllocator;

	KernelVirtualAddressAllocator allocator;
	allocator.Init(0x1000, 0x9000);
	ROCINANTE_EXPECT_TRUE(ctx, allocator.IsInitialized());

	// Split behavior with alignment-induced prefix.
	const auto aligned_or = allocator.Allocate(0x1000, 0x2000);
	ROCINANTE_EXPECT_TRUE(ctx, aligned_or.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, aligned_or.value(), 0x2000);

	// Should still be able to allocate from the prefix [0x1000,0x2000).
	const auto prefix_or = allocator.Allocate(0x1000, 0x1000);
	ROCINANTE_EXPECT_TRUE(ctx, prefix_or.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, prefix_or.value(), 0x1000);

	// Free and coalesce back into one contiguous range.
	ROCINANTE_EXPECT_TRUE(ctx, allocator.Free(prefix_or.value(), 0x1000));
	ROCINANTE_EXPECT_TRUE(ctx, allocator.Free(aligned_or.value(), 0x1000));

	const auto merged_or = allocator.Allocate(0x2000, 0x1000);
	ROCINANTE_EXPECT_TRUE(ctx, merged_or.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, merged_or.value(), 0x1000);

	// Double free should be rejected.
	ROCINANTE_EXPECT_TRUE(ctx, allocator.Free(merged_or.value(), 0x2000));
	ROCINANTE_EXPECT_TRUE(ctx, !allocator.Free(merged_or.value(), 0x2000));
}

} // namespace

void TestEntry_KernelVirtualAddressAllocator_AllocateFreeCoalesce(TestContext* ctx) {
	Test_KernelVirtualAddressAllocator_AllocateFreeCoalesce(ctx);
}

} // namespace Rocinante::Testing

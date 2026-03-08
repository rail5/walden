/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/pmm.h>
#include <src/memory/paging.h>
#include <src/memory/vmm_unmap.h>
#include <src/memory/vm_object.h>
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

static void Test_VMM_AnonymousVmObject_Ownership(TestContext* ctx) {
	using Rocinante::Memory::AnonymousVmObject;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::GetPhysicalMemoryManager;
	using Rocinante::Memory::PhysicalMemoryManager;

	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsablePages = 128;
	static constexpr std::size_t kUsableSizeBytes = kUsablePages * PhysicalMemoryManager::kPageSizeBytes;

	// Keep kernel and DTB out of the tracked span so this test focuses on
	// AnonymousVmObject ownership behavior.
	static constexpr std::uintptr_t kKernelBase = 0x00400000;
	static constexpr std::uintptr_t kKernelEnd = 0x00401000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00500000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	AnonymousVmObject object;
	ROCINANTE_EXPECT_TRUE(ctx, object.IsEmpty());

	const std::size_t free_before = pmm.FreePages();

	const auto page0_first = object.GetOrCreateFrameForPageOffset(&pmm, 0);
	ROCINANTE_EXPECT_TRUE(ctx, page0_first.has_value());
	if (!page0_first.has_value()) return;
	ROCINANTE_EXPECT_TRUE(ctx, page0_first.value().created);
	const std::uintptr_t pa0 = page0_first.value().physical_page_base;

	// Storage policy note:
	// AnonymousVmObject allocates metadata pages (radix directory + one block page)
	// on first use, in addition to the payload frame itself.
	// Therefore, the first GetOrCreate consumes 3 PMM pages total.
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before - 3);
	{
		const auto ref = pmm.ReferenceCountForPhysical(pa0);
		ROCINANTE_EXPECT_TRUE(ctx, ref.has_value());
		if (ref.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref.value(), 1);
		}
	}

	// Second access to the same page offset returns the same frame without a new allocation.
	const auto page0_second = object.GetOrCreateFrameForPageOffset(&pmm, 0);
	ROCINANTE_EXPECT_TRUE(ctx, page0_second.has_value());
	if (!page0_second.has_value()) return;
	ROCINANTE_EXPECT_TRUE(ctx, !page0_second.value().created);
	ROCINANTE_EXPECT_EQ_U64(ctx, page0_second.value().physical_page_base, pa0);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before - 3);

	// A different page offset creates a different owned frame.
	const auto page1_first = object.GetOrCreateFrameForPageOffset(&pmm, 1);
	ROCINANTE_EXPECT_TRUE(ctx, page1_first.has_value());
	if (!page1_first.has_value()) return;
	ROCINANTE_EXPECT_TRUE(ctx, page1_first.value().created);
	const std::uintptr_t pa1 = page1_first.value().physical_page_base;
	ROCINANTE_EXPECT_TRUE(ctx, pa1 != pa0);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before - 4);

	// Force the radix directory to grow beyond a single-level directory by
	// accessing a page offset whose block index exceeds 512.
	//
	// page_offset = (513 blocks * 512 pages/block) + 0 pages.
	static constexpr std::size_t kPagesPerBlock = 512;
	static constexpr std::size_t kFarBlockIndex = 513;
	static constexpr std::size_t kFarOffset = kFarBlockIndex * kPagesPerBlock;
	const auto far_first = object.GetOrCreateFrameForPageOffset(&pmm, kFarOffset);
	ROCINANTE_EXPECT_TRUE(ctx, far_first.has_value());
	if (!far_first.has_value()) return;
	ROCINANTE_EXPECT_TRUE(ctx, far_first.value().created);
	const std::uintptr_t pa_far = far_first.value().physical_page_base;
	ROCINANTE_EXPECT_TRUE(ctx, pa_far != pa0);
	ROCINANTE_EXPECT_TRUE(ctx, pa_far != pa1);

	// Directory growth consumes 3 more metadata pages (new root + one child directory + new block)
	// plus the payload frame.
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before - 8);

	ROCINANTE_EXPECT_TRUE(ctx, object.ReleaseAllOwnedFrames(&pmm));
	ROCINANTE_EXPECT_TRUE(ctx, object.IsEmpty());
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before);
	{
		const auto ref0 = pmm.ReferenceCountForPhysical(pa0);
		const auto ref1 = pmm.ReferenceCountForPhysical(pa1);
		const auto ref_far = pmm.ReferenceCountForPhysical(pa_far);
		ROCINANTE_EXPECT_TRUE(ctx, ref0.has_value());
		ROCINANTE_EXPECT_TRUE(ctx, ref1.has_value());
		ROCINANTE_EXPECT_TRUE(ctx, ref_far.has_value());
		if (ref0.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref0.value(), 0);
		}
		if (ref1.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref1.value(), 0);
		}
		if (ref_far.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref_far.value(), 0);
		}
	}
}

static void Test_VMM_UnmapVma4KiB_ReleasesAnonymousFrames(TestContext* ctx) {
	using Rocinante::Memory::AnonymousVmObject;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::GetPhysicalMemoryManager;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::VirtualMemoryArea;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::Translate;
	using Rocinante::Memory::Paging::UnmapPage4KiB;
	using Rocinante::Memory::VmmUnmap::UnmapVma4KiB;

	// This test exercises the software page table builder/walker.
	// It does not enable paging in hardware.

	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsableSizeBytes = 256 * PhysicalMemoryManager::kPageSizeBytes;

	// Keep kernel/DTB reservations outside our usable range for this test.
	static constexpr std::uintptr_t kKernelBase = 0x00400000;
	static constexpr std::uintptr_t kKernelEnd = 0x00401000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00500000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	const auto root_or = AllocateRootPageTable(&pmm);
	ROCINANTE_EXPECT_TRUE(ctx, root_or.has_value());
	if (!root_or.has_value()) return;

	static constexpr std::uintptr_t kVirtualPageBase = 0x0000000000200000ull;
	static_assert((kVirtualPageBase % PhysicalMemoryManager::kPageSizeBytes) == 0);

	AnonymousVmObject object;
	VirtualMemoryArea vma;
	vma.virtual_base = kVirtualPageBase;
	vma.virtual_limit = kVirtualPageBase + PhysicalMemoryManager::kPageSizeBytes;
	vma.backing_type = VirtualMemoryArea::BackingType::Anonymous;
	vma.anonymous_object = &object;
	vma.owns_frames = true;

	static constexpr PagePermissions kPermissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};
	vma.permissions = kPermissions;

	const auto frame_or = object.GetOrCreateFrameForPageOffset(&pmm, 0);
	ROCINANTE_EXPECT_TRUE(ctx, frame_or.has_value());
	if (!frame_or.has_value()) return;
	const std::uintptr_t physical_page_base = frame_or.value().physical_page_base;

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root_or.value(), kVirtualPageBase, physical_page_base, kPermissions));

	// Sanity: mapped and tracked.
	ROCINANTE_EXPECT_TRUE(ctx, Translate(root_or.value(), kVirtualPageBase).has_value());
	{
		const auto map_count = pmm.MapCountForPhysical(physical_page_base);
		ROCINANTE_EXPECT_TRUE(ctx, map_count.has_value());
		if (map_count.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, map_count.value(), 1);
		}
	}
	{
		const auto ref_count = pmm.ReferenceCountForPhysical(physical_page_base);
		ROCINANTE_EXPECT_TRUE(ctx, ref_count.has_value());
		if (ref_count.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref_count.value(), 1);
		}
	}

	// Demonstrate the invariant boundary explicitly:
	// - Unmapping removes the leaf PTE and decrements map_count.
	// - The frame is NOT freeable while the object still holds ref_count.
	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(&pmm, root_or.value(), kVirtualPageBase));
	ROCINANTE_EXPECT_TRUE(ctx, !Translate(root_or.value(), kVirtualPageBase).has_value());
	{
		const auto map_count = pmm.MapCountForPhysical(physical_page_base);
		ROCINANTE_EXPECT_TRUE(ctx, map_count.has_value());
		if (map_count.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, map_count.value(), 0);
		}
	}
	{
		const auto ref_count = pmm.ReferenceCountForPhysical(physical_page_base);
		ROCINANTE_EXPECT_TRUE(ctx, ref_count.has_value());
		if (ref_count.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref_count.value(), 1);
		}
	}

	// Remap and then exercise the VMM unmap helper, which also releases object ownership.
	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root_or.value(), kVirtualPageBase, physical_page_base, kPermissions));
	ROCINANTE_EXPECT_TRUE(ctx, UnmapVma4KiB(&pmm, root_or.value(), vma));

	ROCINANTE_EXPECT_TRUE(ctx, !Translate(root_or.value(), kVirtualPageBase).has_value());
	{
		const auto map_count = pmm.MapCountForPhysical(physical_page_base);
		ROCINANTE_EXPECT_TRUE(ctx, map_count.has_value());
		if (map_count.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, map_count.value(), 0);
		}
	}
	{
		const auto ref_count = pmm.ReferenceCountForPhysical(physical_page_base);
		ROCINANTE_EXPECT_TRUE(ctx, ref_count.has_value());
		if (ref_count.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, ref_count.value(), 0);
		}
	}
	ROCINANTE_EXPECT_TRUE(ctx, object.IsEmpty());
}

} // namespace

void TestEntry_VMM_VMA_InsertLookup(TestContext* ctx) {
	Test_VMM_VMA_InsertLookup(ctx);
}

void TestEntry_VMM_AnonymousVmObject_Ownership(TestContext* ctx) {
	Test_VMM_AnonymousVmObject_Ownership(ctx);
}

void TestEntry_VMM_UnmapVma4KiB_ReleasesAnonymousFrames(TestContext* ctx) {
	Test_VMM_UnmapVma4KiB_ReleasesAnonymousFrames(ctx);
}

} // namespace Rocinante::Testing

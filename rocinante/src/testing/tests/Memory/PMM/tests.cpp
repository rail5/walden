/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpucfg.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/pmm.h>

#include <cstddef>
#include <cstdint>

namespace Rocinante::Testing {

namespace {

static constexpr std::size_t AlignUpSizeBytes(std::size_t value, std::size_t alignment) {
	return (value + (alignment - 1)) & ~(alignment - 1);
}

static constexpr std::size_t BitmapReservedPagesForTrackedPages(std::size_t tracked_page_count) {
	const std::size_t bit_count = tracked_page_count;
	const std::size_t byte_count = (bit_count + 7) / 8;
	return AlignUpSizeBytes(byte_count, Rocinante::Memory::PhysicalMemoryManager::kPageSizeBytes) /
		Rocinante::Memory::PhysicalMemoryManager::kPageSizeBytes;
}

static constexpr std::size_t FrameMetadataReservedPagesForTrackedPages(std::size_t tracked_page_count) {
	const std::size_t raw_size_bytes =
		tracked_page_count * Rocinante::Memory::PhysicalMemoryManager::kPageFrameMetadataSizeBytes;
	return AlignUpSizeBytes(raw_size_bytes, Rocinante::Memory::PhysicalMemoryManager::kPageSizeBytes) /
		Rocinante::Memory::PhysicalMemoryManager::kPageSizeBytes;
}

static void Test_PMM_RespectsReservedKernelAndDTB(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	// Construct a synthetic boot memory map.
	//
	// Layout (all addresses are physical):
	// - Usable RAM: 16 pages (64 KiB)
	// - Reserved region: 2 pages inside usable RAM
	// - Kernel image: 4 pages inside usable RAM
	// - DTB blob: 1 page inside usable RAM
	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsableSizeBytes = 16 * PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uintptr_t kReservedBase = 0x00108000;
	static constexpr std::size_t kReservedSizeBytes = 2 * PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uintptr_t kKernelBase = 0x00100000;
	static constexpr std::uintptr_t kKernelEnd = 0x00104000;

	static constexpr std::uintptr_t kDeviceTreeBase = 0x0010C000;
	static constexpr std::size_t kDeviceTreeSizeBytes = 1 * PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();

	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kReservedBase, .size_bytes = kReservedSizeBytes, .type = BootMemoryRegion::Type::Reserved}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	// Expected free pages: total usable (16) minus reserved (2) minus kernel (4) minus DTB (1)
	// minus PMM bitmap storage (at least 1 page) minus PMM per-frame metadata = 7.
	static constexpr std::size_t kExpectedTotalPages = 16;
	static constexpr std::size_t kExpectedFreePages =
		kExpectedTotalPages - 2 - 4 - 1 -
		BitmapReservedPagesForTrackedPages(kExpectedTotalPages) -
		FrameMetadataReservedPagesForTrackedPages(kExpectedTotalPages);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.TotalPages(), kExpectedTotalPages);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), kExpectedFreePages);

	// Allocate all pages and ensure no allocation falls into reserved/kernel/DTB.
	std::size_t allocations = 0;
	while (true) {
		const auto page = pmm.AllocatePage();
		if (!page.has_value()) break;
		const std::uintptr_t physical = page.value();
		allocations++;

		ROCINANTE_EXPECT_TRUE(ctx, (physical % PhysicalMemoryManager::kPageSizeBytes) == 0);
		ROCINANTE_EXPECT_TRUE(ctx, physical >= kUsableBase);
		ROCINANTE_EXPECT_TRUE(ctx, physical < (kUsableBase + kUsableSizeBytes));

		const bool in_reserved = (physical >= kReservedBase) && (physical < (kReservedBase + kReservedSizeBytes));
		const bool in_kernel = (physical >= kKernelBase) && (physical < kKernelEnd);
		const bool in_dtb = (physical >= kDeviceTreeBase) && (physical < (kDeviceTreeBase + kDeviceTreeSizeBytes));
		ROCINANTE_EXPECT_TRUE(ctx, !in_reserved);
		ROCINANTE_EXPECT_TRUE(ctx, !in_kernel);
		ROCINANTE_EXPECT_TRUE(ctx, !in_dtb);
	}

	ROCINANTE_EXPECT_EQ_U64(ctx, allocations, kExpectedFreePages);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), 0);
}

static void Test_PMM_DoesNotClobberReservedDuringBitmapPlacement(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	// Regression guard:
	// The PMM bitmap is carved out of a UsableRAM region. If bitmap placement
	// does not respect DTB-reserved regions, initialization can silently overwrite
	// reserved physical memory.
	//
	// Construct a synthetic map where the first page of UsableRAM is Reserved.
	// If the bitmap placement code incorrectly selects the start of the UsableRAM
	// region, it will overwrite our poison pattern.
	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsableSizeBytes = 64 * PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uintptr_t kReservedBase = kUsableBase;
	static constexpr std::size_t kReservedSizeBytes = 1 * PhysicalMemoryManager::kPageSizeBytes;

	// Keep kernel/DTB outside the usable region so bitmap placement is not bumped
	// for unrelated reasons.
	static constexpr std::uintptr_t kKernelBase = 0x00400000;
	static constexpr std::uintptr_t kKernelEnd = 0x00401000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00500000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uint8_t kPoison = 0x5A;
	volatile std::uint8_t* reserved = reinterpret_cast<volatile std::uint8_t*>(kReservedBase);
	for (std::size_t i = 0; i < kReservedSizeBytes; i++) {
		reserved[i] = kPoison;
	}

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kReservedBase, .size_bytes = kReservedSizeBytes, .type = BootMemoryRegion::Type::Reserved}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	for (std::size_t i = 0; i < kReservedSizeBytes; i++) {
		ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(reserved[i]), static_cast<std::uint64_t>(kPoison));
	}
}

static std::uint32_t CPUCFG_ReadViaInstruction(void*, std::uint32_t word_number) {
	std::uint32_t value = 0;
	asm volatile(
		"cpucfg %0, %1"
		: "=r"(value)
		: "r"(word_number)
	);
	return value;
}

struct FakeCPUCFGBackendForPMM final {
	std::uint32_t word1 = 0;

	static std::uint32_t Read(void* context, std::uint32_t word_number) {
		auto* self = static_cast<FakeCPUCFGBackendForPMM*>(context);
		if (word_number == 0x1) return self->word1;
		return 0;
	}
};

static void Test_PMM_ClampsTrackedRangeToPALEN(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	// Regression guard:
	// The PMM should never track/allocate physical pages beyond the CPU's
	// architecturally-supported physical address width (PALEN).
	//
	// We inject a small PALEN so the clamp is observable in a tiny synthetic map.
	// Pick PALEN=20 => physical address space is 2^20 bytes = 1 MiB.
	static constexpr std::uint32_t kPALEN = 20;

	// Encode CPUCFG word 0x1 fields needed by PhysicalAddressBits().
	// - ARCH[1:0] = 2 (LA64)
	// - PALEN[11:4] = PALEN-1
	static constexpr std::uint32_t kArchLA64 = 2;
	static constexpr std::uint32_t kArchShift = 0;
	static constexpr std::uint32_t kPALENMinus1Shift = 4;
	static constexpr std::uint32_t kWord1 =
		(kArchLA64 << kArchShift) |
		((kPALEN - 1u) << kPALENMinus1Shift);

	FakeCPUCFGBackendForPMM fake;
	fake.word1 = kWord1;

	auto& cpucfg = Rocinante::GetCPUCFG();
	cpucfg.SetBackend(Rocinante::CPUCFGBackend{.context = &fake, .read_word = &FakeCPUCFGBackendForPMM::Read});

	BootMemoryMap map;
	map.Clear();

	// Report 4 MiB of UsableRAM starting at 0.
	static constexpr std::uintptr_t kUsableBase = 0x00000000;
	static constexpr std::size_t kUsableSizeBytes = 4 * 1024 * 1024;
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, 0, 0, 0, 0));

	// With PALEN=20, the PMM must clamp tracking to [0, 1MiB): 256 pages.
	static constexpr std::size_t kExpectedTotalPages = (1 * 1024 * 1024) / PhysicalMemoryManager::kPageSizeBytes;
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.TotalPages(), kExpectedTotalPages);

	// Free pages: total minus bitmap-storage pages minus per-frame metadata pages
	// minus the zero page reservation.
	static constexpr std::size_t kExpectedFreePages =
		kExpectedTotalPages -
		BitmapReservedPagesForTrackedPages(kExpectedTotalPages) -
		FrameMetadataReservedPagesForTrackedPages(kExpectedTotalPages) -
		1;
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), kExpectedFreePages);

	// Restore CPUCFG backend to the real instruction path for subsequent tests.
	cpucfg.SetBackend(Rocinante::CPUCFGBackend{.context = nullptr, .read_word = &CPUCFG_ReadViaInstruction});
	// Ensure future accesses see real CPU values.
	cpucfg.ResetCache();
}

static void Test_PMM_BitmapPlacement_RespectsPALEN(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	// Regression guard:
	// Even if the boot map reports a UsableRAM region above 2^PALEN, the PMM must
	// not place its bitmap metadata there.
	static constexpr std::uint32_t kPALEN = 20; // 1 MiB physical address space.

	static constexpr std::uint32_t kArchLA64 = 2;
	static constexpr std::uint32_t kWord1 = (kArchLA64 << 0) | ((kPALEN - 1u) << 4);

	FakeCPUCFGBackendForPMM fake;
	fake.word1 = kWord1;

	auto& cpucfg = Rocinante::GetCPUCFG();
	cpucfg.SetBackend(Rocinante::CPUCFGBackend{.context = &fake, .read_word = &FakeCPUCFGBackendForPMM::Read});

	BootMemoryMap map;
	map.Clear();

	// Two UsableRAM regions:
	// - First is entirely above max physical range (>= 2 MiB).
	// - Second is within [0, 1 MiB).
	static constexpr std::uintptr_t kOutOfRangeUsableBase = 0x00200000;
	static constexpr std::size_t kOutOfRangeUsableSizeBytes = 0x00100000;
	static constexpr std::uintptr_t kInRangeUsableBase = 0x00000000;
	static constexpr std::size_t kInRangeUsableSizeBytes = 0x00100000;

	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kOutOfRangeUsableBase, .size_bytes = kOutOfRangeUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kInRangeUsableBase, .size_bytes = kInRangeUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, 0, 0, 0, 0));

	// With PALEN=20, tracked range must still be exactly 1 MiB.
	static constexpr std::size_t kExpectedTotalPages = (1 * 1024 * 1024) / PhysicalMemoryManager::kPageSizeBytes;
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.TotalPages(), kExpectedTotalPages);

	// Restore CPUCFG backend.
	cpucfg.SetBackend(Rocinante::CPUCFGBackend{.context = nullptr, .read_word = &CPUCFG_ReadViaInstruction});
	cpucfg.ResetCache();
}

static void Test_PMM_Initialize_SingleUsableRegionContainingKernelAndDTB(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	// Regression: real bring-up often reports a single large UsableRAM region,
	// with the kernel image and DTB ranges living inside it.
	//
	// If the PMM's bitmap-carving logic only tries the start of the UsableRAM
	// region and rejects overlaps, PMM initialization fails.
	static constexpr std::uintptr_t kUsableBase = 0x00000000;
	static constexpr std::size_t kUsableSizeBytes = 1024 * PhysicalMemoryManager::kPageSizeBytes; // 4 MiB

	// Match the real linker placement in src/asm/linker.ld: the kernel is linked
	// to start at 0x00200000 (2 MiB), leaving 0..1 MiB for QEMU boot info/DTB.
	static constexpr std::uintptr_t kKernelBase = 0x00200000;
	static constexpr std::uintptr_t kKernelEnd = 0x00240000; // 256 KiB

	static constexpr std::uintptr_t kDeviceTreeBase = 0x00100000;
	static constexpr std::size_t kDeviceTreeSizeBytes = 0x00100000; // 1 MiB

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	// Usable pages: 1024.
	// Reserved pages:
	// - DTB: 1 MiB = 256 pages
	// - Kernel: 256 KiB = 64 pages
	// - PMM bitmap storage: for 1024 tracked pages => 128 bytes, which occupies 1 page once page-granular reserved
	// - PMM per-frame metadata: for 1024 tracked pages => 16 KiB, which occupies 4 pages once page-granular reserved
	// - Zero page: explicitly reserved by PMM policy (since kernel is not at physical 0)
	static constexpr std::size_t kExpectedTotalPages = 1024;
	static constexpr std::size_t kExpectedFreePages =
		kExpectedTotalPages - 256 - 64 -
		BitmapReservedPagesForTrackedPages(kExpectedTotalPages) -
		FrameMetadataReservedPagesForTrackedPages(kExpectedTotalPages) -
		1;
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.TotalPages(), kExpectedTotalPages);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), kExpectedFreePages);
}

static void Test_PMM_PageFrameNumberConversions(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsablePages = 64;
	static constexpr std::size_t kUsableSizeBytes = kUsablePages * PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uintptr_t kKernelBase = 0x00400000;
	static constexpr std::uintptr_t kKernelEnd = 0x00401000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00500000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	const std::uintptr_t tracked_base = pmm.TrackedPhysicalBase();
	const std::uintptr_t tracked_limit = pmm.TrackedPhysicalLimit();
	const std::size_t tracked_pages = pmm.TotalPages();
	ROCINANTE_EXPECT_TRUE(ctx, tracked_pages != 0);

	const auto pfn0 = pmm.PageFrameNumberFromPhysical(tracked_base);
	ROCINANTE_EXPECT_TRUE(ctx, pfn0.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, pfn0.value(), 0);

	const auto pfn_last = pmm.PageFrameNumberFromPhysical(tracked_base + ((tracked_pages - 1) * PhysicalMemoryManager::kPageSizeBytes));
	ROCINANTE_EXPECT_TRUE(ctx, pfn_last.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, pfn_last.value(), tracked_pages - 1);

	ROCINANTE_EXPECT_TRUE(ctx, !pmm.PageFrameNumberFromPhysical(tracked_base + 1).has_value());
	ROCINANTE_EXPECT_TRUE(ctx, !pmm.PageFrameNumberFromPhysical(tracked_limit).has_value());

	const auto phys0 = pmm.PhysicalFromPageFrameNumber(0);
	ROCINANTE_EXPECT_TRUE(ctx, phys0.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, phys0.value(), tracked_base);

	const auto phys_last = pmm.PhysicalFromPageFrameNumber(tracked_pages - 1);
	ROCINANTE_EXPECT_TRUE(ctx, phys_last.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, phys_last.value(), tracked_base + ((tracked_pages - 1) * PhysicalMemoryManager::kPageSizeBytes));

	ROCINANTE_EXPECT_TRUE(ctx, !pmm.PhysicalFromPageFrameNumber(tracked_pages).has_value());
}

static void Test_PMM_ReferenceCount_RetainRelease(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;

	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsablePages = 64;
	static constexpr std::size_t kUsableSizeBytes = kUsablePages * PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uintptr_t kKernelBase = 0x00400000;
	static constexpr std::uintptr_t kKernelEnd = 0x00401000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00500000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	const std::size_t free_before = pmm.FreePages();
	const auto physical_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, physical_or.has_value());
	if (!physical_or.has_value()) return;
	const std::uintptr_t physical_page_base = physical_or.value();
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before - 1);

	const auto ref0 = pmm.ReferenceCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, ref0.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, ref0.value(), 1);

	ROCINANTE_EXPECT_TRUE(ctx, pmm.RetainPhysicalPage(physical_page_base));
	const auto ref1 = pmm.ReferenceCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, ref1.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, ref1.value(), 2);

	ROCINANTE_EXPECT_TRUE(ctx, pmm.ReleasePhysicalPage(physical_page_base));
	const auto ref2 = pmm.ReferenceCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, ref2.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, ref2.value(), 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before - 1);

	// Policy: the final release must fail if the page is still mapped.
	ROCINANTE_EXPECT_TRUE(ctx, pmm.IncrementMapCountForPhysical(physical_page_base));
	ROCINANTE_EXPECT_TRUE(ctx, !pmm.ReleasePhysicalPage(physical_page_base));
	const auto ref3 = pmm.ReferenceCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, ref3.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, ref3.value(), 1);
	ROCINANTE_EXPECT_TRUE(ctx, pmm.DecrementMapCountForPhysical(physical_page_base));

	ROCINANTE_EXPECT_TRUE(ctx, pmm.FreePage(physical_page_base));
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before);
}

} // namespace

void TestEntry_PMM_RespectsReservedKernelAndDTB(TestContext* ctx) {
	Test_PMM_RespectsReservedKernelAndDTB(ctx);
}

void TestEntry_PMM_DoesNotClobberReservedDuringBitmapPlacement(TestContext* ctx) {
	Test_PMM_DoesNotClobberReservedDuringBitmapPlacement(ctx);
}

void TestEntry_PMM_ClampsTrackedRangeToPALEN(TestContext* ctx) {
	Test_PMM_ClampsTrackedRangeToPALEN(ctx);
}

void TestEntry_PMM_BitmapPlacement_RespectsPALEN(TestContext* ctx) {
	Test_PMM_BitmapPlacement_RespectsPALEN(ctx);
}

void TestEntry_PMM_Initialize_SingleUsableRegionContainingKernelAndDTB(TestContext* ctx) {
	Test_PMM_Initialize_SingleUsableRegionContainingKernelAndDTB(ctx);
}

void TestEntry_PMM_PageFrameNumberConversions(TestContext* ctx) {
	Test_PMM_PageFrameNumberConversions(ctx);
}

void TestEntry_PMM_ReferenceCount_RetainRelease(TestContext* ctx) {
	Test_PMM_ReferenceCount_RetainRelease(ctx);
}

} // namespace Rocinante::Testing

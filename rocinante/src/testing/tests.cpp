/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <cstddef>
#include <cstdint>

#include <src/sp/cpucfg.h>
#include <src/trap.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/pmm.h>
#include <src/memory/paging.h>

namespace Rocinante::Testing {

namespace {

// Read the CPU time counter (LoongArch `rdtime.d`).
//
// Returns: monotonically increasing time-counter ticks.
static inline std::uint64_t ReadTimeCounterTicks() {
	std::uint64_t value;
	asm volatile("rdtime.d %0, $zero" : "=r"(value));
	return value;
}

struct FakeCPUCFGBackend final {
	// LoongArch CPUCFG currently defines words 0x0..0x14.
	static constexpr std::uint32_t kCPUCFGWordCount = 0x15;

	std::uint32_t words[kCPUCFGWordCount]{};

	static std::uint32_t Read(void* context, std::uint32_t word_number) {
		auto* self = static_cast<FakeCPUCFGBackend*>(context);
		if (word_number < kCPUCFGWordCount) return self->words[word_number];
		return 0;
	}
};

static void Test_CPUCFG_FakeBackend_DecodesWord1(TestContext* ctx) {
	CPUCFG cpucfg;
	FakeCPUCFGBackend fake;

	// Construct CPUCFG word 0x1 using the architectural bit layout.
	//
	// Fields (LoongArch CPUCFG word 1):
	// - ARCH in bits [1:0]
	// - PALEN-1 (physical address bits minus 1) in bits [11:4]
	// - VALEN-1 (virtual address bits minus 1) in bits [19:12]
	static constexpr std::uint32_t kCPUCFGWordIndex = 0x1;
	static constexpr std::uint32_t kArchShift = 0;
	static constexpr std::uint32_t kPhysicalAddressBitsMinus1Shift = 4;
	static constexpr std::uint32_t kVirtualAddressBitsMinus1Shift = 12;

	static constexpr std::uint32_t kArchLA64 = 2;
	static constexpr std::uint32_t kPhysicalAddressBitsMinus1 = 47;
	static constexpr std::uint32_t kVirtualAddressBitsMinus1 = 47;

	static constexpr std::uint32_t kWord1 =
		(kArchLA64 << kArchShift) |
		(kPhysicalAddressBitsMinus1 << kPhysicalAddressBitsMinus1Shift) |
		(kVirtualAddressBitsMinus1 << kVirtualAddressBitsMinus1Shift);

	fake.words[kCPUCFGWordIndex] = kWord1;

	cpucfg.SetBackend(CPUCFGBackend{.context = &fake, .read_word = &FakeCPUCFGBackend::Read});

	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(cpucfg.Arch()), static_cast<std::uint64_t>(CPUCFG::Architecture::LA64));
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.PhysicalAddressBits(), kPhysicalAddressBitsMinus1 + 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.VirtualAddressBits(), kVirtualAddressBitsMinus1 + 1);

	// Word 0x1 should be cached after the first access.
	(void)cpucfg.VirtualAddressBits();
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.BackendReadCount(), 1);
}

static void Test_CPUCFG_FakeBackend_CachesWords(TestContext* ctx) {
	CPUCFG cpucfg;
	FakeCPUCFGBackend fake;

	static constexpr std::uint32_t kCPUCFGWord0Index = 0x0;
	static constexpr std::uint32_t kProcessorIDWordValue = 0x12345678u;

	fake.words[kCPUCFGWord0Index] = kProcessorIDWordValue;
	cpucfg.SetBackend(CPUCFGBackend{.context = &fake, .read_word = &FakeCPUCFGBackend::Read});

	(void)cpucfg.ProcessorID();
	(void)cpucfg.ProcessorID();
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.BackendReadCount(), 1);

	cpucfg.ResetCache();
	(void)cpucfg.ProcessorID();
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.BackendReadCount(), 1);
}

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

static void Test_Interrupts_TimerIRQ_DeliversAndClears(TestContext* ctx) {
	ResetTrapObservations();

	Rocinante::Trap::DisableInterrupts();
	Rocinante::Trap::MaskAllInterruptLines();

	// The units here are timer ticks (hardware-defined). The goal is not a
	// precise delay; it is to reliably trigger a timer interrupt in QEMU.
	static constexpr std::uint64_t kOneShotTimerDelayTicks = 100000;
	Rocinante::Trap::StartOneShotTimerTicks(kOneShotTimerDelayTicks);
	Rocinante::Trap::UnmaskTimerInterruptLine();
	Rocinante::Trap::EnableInterrupts();

	// We need a timeout so a broken interrupt path fails loudly instead of
	// hanging the kernel forever.
	//
	// The time counter frequency is platform/QEMU dependent; choose a generous
	// timeout in time-counter ticks.
	static constexpr std::uint64_t kTimeoutTimeCounterTicks = 50000000ull;

	const std::uint64_t start_time_ticks = ReadTimeCounterTicks();
	while (!TimerInterruptObserved()) {
		const std::uint64_t now_ticks = ReadTimeCounterTicks();
		if ((now_ticks - start_time_ticks) > kTimeoutTimeCounterTicks) {
			break;
		}
		asm volatile("nop" ::: "memory");
	}

	Rocinante::Trap::DisableInterrupts();

	ROCINANTE_EXPECT_TRUE(ctx, TimerInterruptObserved());
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

	// Expected free pages: total usable (16) minus reserved (2) minus kernel (4) minus DTB (1) = 9.
	static constexpr std::size_t kExpectedTotalPages = 16;
	static constexpr std::size_t kExpectedFreePages = 9;
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

static void Test_Paging_MapTranslateUnmap(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::Translate;
	using Rocinante::Memory::Paging::UnmapPage4KiB;

	// This test exercises the software page table builder/walker.
	// It does not enable paging in hardware.

	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsableSizeBytes = 128 * PhysicalMemoryManager::kPageSizeBytes;

	// Keep kernel/DTB reservations outside our usable range for this test.
	static constexpr std::uintptr_t kKernelBase = 0x00400000;
	static constexpr std::uintptr_t kKernelEnd = 0x00401000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00500000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	const auto root = AllocateRootPageTable(&pmm);
	ROCINANTE_EXPECT_TRUE(ctx, root.has_value());

	// Choose a canonical 48-bit high-half virtual address.
	// Per the LoongArch spec, an implemented LA64 virtual address range is
	// 0 -> 2^VALEN-1. Keep the test address within that range.
	static constexpr std::uintptr_t kVirtualPageBase = 0x0000000000100000ull;
	const auto physical_page = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, physical_page.has_value());
	const std::uintptr_t physical_page_base = physical_page.value();

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root.value(), kVirtualPageBase, physical_page_base, permissions));

	const auto translated = Translate(root.value(), kVirtualPageBase);
	ROCINANTE_EXPECT_TRUE(ctx, translated.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, translated.value(), physical_page_base);

	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(root.value(), kVirtualPageBase));
	const auto translated_after_unmap = Translate(root.value(), kVirtualPageBase);
	ROCINANTE_EXPECT_TRUE(ctx, !translated_after_unmap.has_value());
}

static void Test_Paging_RespectsVALENAndPALEN(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::Translate;

	// IMPORTANT:
	// The kernel is linked at 0x00200000 (see src/asm/linker.ld). Keep synthetic
	// "usable RAM" well away from the kernel image, otherwise the PMM may
	// allocate pages that overwrite the running test binary.
	static constexpr std::uintptr_t kUsableBase = 0x01000000;
	static constexpr std::size_t kUsableSizeBytes = 128 * PhysicalMemoryManager::kPageSizeBytes;

	static constexpr std::uintptr_t kKernelBase = 0x00600000;
	static constexpr std::uintptr_t kKernelEnd = 0x00601000;
	static constexpr std::uintptr_t kDeviceTreeBase = 0x00700000;
	static constexpr std::size_t kDeviceTreeSizeBytes = PhysicalMemoryManager::kPageSizeBytes;

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(map, kKernelBase, kKernelEnd, kDeviceTreeBase, kDeviceTreeSizeBytes));

	const auto root = AllocateRootPageTable(&pmm);
	ROCINANTE_EXPECT_TRUE(ctx, root.has_value());

	const auto physical_page = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, physical_page.has_value());
	const std::uintptr_t physical_page_base = physical_page.value();

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	// Pick a smaller-than-typical address width to prove we do not hard-code 48.
	// 39-bit virtual addresses => canonical addresses must sign-extend bit 38.
	const AddressSpaceBits bits{.virtual_address_bits = 39, .physical_address_bits = 44};

	static constexpr std::uintptr_t kGoodVirtualLow = 0x0000000000100000ull;
	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root.value(), kGoodVirtualLow, physical_page_base, permissions, bits));
	const auto translated = Translate(root.value(), kGoodVirtualLow, bits);
	ROCINANTE_EXPECT_TRUE(ctx, translated.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, translated.value(), physical_page_base);

	// Also accept a canonical high-half address (sign-extended).
	const auto second_physical_page = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, second_physical_page.has_value());
	const std::uintptr_t second_physical_page_base = second_physical_page.value();

	const std::uintptr_t kGoodVirtualHigh =
		(static_cast<std::uintptr_t>(~0ull) << bits.virtual_address_bits) |
		(1ull << (bits.virtual_address_bits - 1)) |
		kGoodVirtualLow;
	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root.value(), kGoodVirtualHigh, second_physical_page_base, permissions, bits));
	const auto translated_high = Translate(root.value(), kGoodVirtualHigh, bits);
	ROCINANTE_EXPECT_TRUE(ctx, translated_high.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, translated_high.value(), second_physical_page_base);

	// VA out of range for VALEN=39.
	const std::uintptr_t bad_virtual = (1ull << 39);
	ROCINANTE_EXPECT_TRUE(ctx, !MapPage4KiB(&pmm, root.value(), bad_virtual, physical_page_base, permissions, bits));

	// PA out of range for PALEN=44.
	const std::uintptr_t bad_physical = physical_page_base | (1ull << 44);
	ROCINANTE_EXPECT_TRUE(ctx, !MapPage4KiB(&pmm, root.value(), kGoodVirtualLow + PhysicalMemoryManager::kPageSizeBytes, bad_physical, permissions, bits));
}

} // namespace

extern const TestCase g_test_cases[] = {
	{"CPUCFG.FakeBackend.DecodesWord1", &Test_CPUCFG_FakeBackend_DecodesWord1},
	{"CPUCFG.FakeBackend.CachesWords", &Test_CPUCFG_FakeBackend_CachesWords},
	{"Traps.BREAK.EntersAndReturns", &Test_Traps_BREAK_EntersAndReturns},
	{"Interrupts.TimerIRQ.DeliversAndClears", &Test_Interrupts_TimerIRQ_DeliversAndClears},
	{"Memory.Paging.MapTranslateUnmap", &Test_Paging_MapTranslateUnmap},
	{"Memory.Paging.RespectsVALENAndPALEN", &Test_Paging_RespectsVALENAndPALEN},
	{"Memory.PMM.RespectsReservedKernelAndDTB", &Test_PMM_RespectsReservedKernelAndDTB},
};

extern const std::size_t g_test_case_count = sizeof(g_test_cases) / sizeof(g_test_cases[0]);

} // namespace Rocinante::Testing

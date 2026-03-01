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
#include <src/memory/paging_hw.h>
#include <src/memory/virtual_layout.h>

extern "C" char _start;
extern "C" char _end;

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

// Shared virtual addresses for paging-hardware tests.
//
// Requirements:
// - Canonical low-half virtual addresses in LA64.
// - Page-aligned.
static constexpr std::uintptr_t kPagingHwScratchVirtualPageBase = 0x0000000100000000ull; // 4 GiB

static std::uintptr_t g_paging_hw_root_page_table_physical = 0;
static std::uint8_t g_paging_hw_virtual_address_bits = 0;
static std::uint8_t g_paging_hw_physical_address_bits = 0;

static std::uintptr_t g_paging_hw_higher_half_stack_guard_virtual_base = 0;
static std::uintptr_t g_paging_hw_higher_half_stack_top = 0;

extern "C" void RocinanteTesting_SwitchStackAndStore(
	std::uintptr_t new_stack_pointer,
	std::uintptr_t store_address,
	std::uint64_t store_value);

asm(R"(
	.text
	.globl RocinanteTesting_SwitchStackAndStore
	.type RocinanteTesting_SwitchStackAndStore, @function
	.p2align 2
RocinanteTesting_SwitchStackAndStore:
	move   $t0, $sp
	move   $sp, $a0
	st.d   $a2, $a1, 0
	move   $sp, $t0
	jr     $ra
)");

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

static void Test_PagingHw_EnablePaging_TlbRefillSmoke(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::MapRange4KiB;
	using Rocinante::Memory::Paging::PagePermissions;

	// This is an end-to-end smoke test for the paging bring-up path.
	//
	// What it guards against:
	// - regressions where enabling paging immediately traps/hangs due to broken
	//   TLBR refill walking (the failure mode we debugged today).
	//
	// WARNING:
	// - This permanently enables paging for the remainder of the test run.
	// - Any tests that run after this must tolerate paging being enabled.
	//
	// Minimal requirements before enabling paging:
	// - identity map the current kernel image (PC + stack + globals)
	// - map UART and syscon MMIO (tests print after enabling; kernel shuts down)
	// - configure PWCL/PWCH/PGD, invalidate the TLB, then flip CRMD.PG/CRMD.DA

	auto& cpucfg = Rocinante::GetCPUCFG();
	ROCINANTE_EXPECT_TRUE(ctx, cpucfg.MMUSupportsPageMappingMode());

	const AddressSpaceBits address_bits{
		.virtual_address_bits = static_cast<std::uint8_t>(cpucfg.VirtualAddressBits()),
		.physical_address_bits = static_cast<std::uint8_t>(cpucfg.PhysicalAddressBits()),
	};

	// Choose a PMM allocation pool that is:
	// - within QEMU RAM (256 MiB @ physical base 0)
	// - away from the kernel image and common low-memory boot blobs (DTB)
	static constexpr std::uintptr_t kUsableBase = 0x01000000; // 16 MiB
	static constexpr std::size_t kUsableSizeBytes = 32u * 1024u * 1024u; // 32 MiB

	const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
	const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);
	ROCINANTE_EXPECT_TRUE(ctx, kernel_physical_end > kernel_physical_base);

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{
		.physical_base = kUsableBase,
		.size_bytes = kUsableSizeBytes,
		.type = BootMemoryRegion::Type::UsableRAM,
	}));

	// We don't have a DTB pointer in the test environment; keep the DTB reservation empty.
	static constexpr std::uintptr_t kDeviceTreeBase = 0;
	static constexpr std::size_t kDeviceTreeSizeBytes = 0;

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(
		map,
		kernel_physical_base,
		kernel_physical_end,
		kDeviceTreeBase,
		kDeviceTreeSizeBytes));

	const auto root = AllocateRootPageTable(&pmm);
	ROCINANTE_EXPECT_TRUE(ctx, root.has_value());
	if (!root.has_value()) return;

	// Expose the paging configuration to later paging-hardware tests.
	//
	// Suite contract: the paging bring-up smoke test is the one that permanently
	// enables paging for the remainder of the run.
	g_paging_hw_root_page_table_physical = root.value().root_physical_address;
	g_paging_hw_virtual_address_bits = address_bits.virtual_address_bits;
	g_paging_hw_physical_address_bits = address_bits.physical_address_bits;

	const PagePermissions kernel_permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::Executable,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	const PagePermissions mmio_permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::StrongUncached,
		.global = true,
	};

	// Identity map the running kernel image.
	const std::uintptr_t kernel_size_bytes = kernel_physical_end - kernel_physical_base;
	const std::size_t kernel_size_rounded =
		static_cast<std::size_t>((kernel_size_bytes + Rocinante::Memory::Paging::kPageSizeBytes - 1) &
			~(Rocinante::Memory::Paging::kPageSizeBytes - 1));
	ROCINANTE_EXPECT_TRUE(ctx, MapRange4KiB(
		&pmm,
		root.value(),
		kernel_physical_base,
		kernel_physical_base,
		kernel_size_rounded,
		kernel_permissions,
		address_bits));

	// Identity map UART16550 MMIO page so test output continues in mapped mode.
	// QEMU LoongArch virt: UART16550 base is 0x1fe001e0.
	static constexpr std::uintptr_t kUartPhysicalBase = 0x1fe001e0ull;
	const std::uintptr_t uart_page_base = kUartPhysicalBase & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	ROCINANTE_EXPECT_TRUE(ctx, MapRange4KiB(
		&pmm,
		root.value(),
		uart_page_base,
		uart_page_base,
		Rocinante::Memory::Paging::kPageSizeBytes,
		mmio_permissions,
		address_bits));

	// Identity map syscon-poweroff MMIO page so kernel_main can shut down after tests.
	// QEMU LoongArch virt: syscon-poweroff uses a syscon at 0x100e001c.
	static constexpr std::uintptr_t kSysconPhysicalBase = 0x100e001cull;
	const std::uintptr_t syscon_page_base = kSysconPhysicalBase & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	ROCINANTE_EXPECT_TRUE(ctx, MapRange4KiB(
		&pmm,
		root.value(),
		syscon_page_base,
		syscon_page_base,
		Rocinante::Memory::Paging::kPageSizeBytes,
		mmio_permissions,
		address_bits));

	// Map a scratch page at a non-identity virtual address so we can force a
	// translation that must be serviced via TLBR refill.
	static constexpr std::uintptr_t kScratchVirtualPageBase = kPagingHwScratchVirtualPageBase;
	const auto scratch_page = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, scratch_page.has_value());
	const std::uintptr_t scratch_physical_page_base = scratch_page.value();

	// Initialize physical memory while still in direct-address mode.
	auto* scratch_physical_u64 = reinterpret_cast<volatile std::uint64_t*>(scratch_physical_page_base);
	*scratch_physical_u64 = 0x1122334455667788ull;

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(
		&pmm,
		root.value(),
		kScratchVirtualPageBase,
		scratch_physical_page_base,
		PagePermissions{
			.access = AccessPermissions::ReadWrite,
			.execute = ExecutePermissions::NoExecute,
			.cache = CacheMode::CoherentCached,
			.global = true,
		},
		address_bits));

	// Map a physmap window that covers the PMM allocation pool.
	//
	// Correctness requirement:
	// Once paging is enabled, software must not dereference physical addresses as
	// pointers. The paging builder/walker accesses page-table pages via the
	// physmap in mapped mode.
	//
	// Therefore, the physmap must cover any physical pages that may hold page
	// tables (root + intermediate levels), which are allocated from the PMM pool.
	const std::uintptr_t physmap_virtual_base =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(kUsableBase, address_bits.virtual_address_bits);

	const PagePermissions physmap_permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	ROCINANTE_EXPECT_TRUE(ctx, MapRange4KiB(
		&pmm,
		root.value(),
		physmap_virtual_base,
		kUsableBase,
		kUsableSizeBytes,
		physmap_permissions,
		address_bits));

	// Map a higher-half stack region with a guard page below it.
	//
	// Purpose:
	// - Provide a mapped stack at a canonical higher-half virtual address.
	// - Leave an unmapped guard page immediately below it.
	// - Later tests can switch to this stack and prove that touching the guard
	//   page faults with a paging exception (PIL/PIS) and a useful BADV.
	{
		static constexpr std::size_t kHigherHalfStackGuardPageCount = 1;
		static constexpr std::size_t kHigherHalfStackMappedPageCount = 4;

		const std::uintptr_t higher_half_base =
			Rocinante::Memory::VirtualLayout::KernelHigherHalfBase(address_bits.virtual_address_bits);
		const std::uintptr_t stack_guard_virtual_base = higher_half_base;
		const std::uintptr_t stack_virtual_base =
			stack_guard_virtual_base +
			(kHigherHalfStackGuardPageCount * Rocinante::Memory::Paging::kPageSizeBytes);
		const std::uintptr_t stack_virtual_top =
			stack_virtual_base +
			(kHigherHalfStackMappedPageCount * Rocinante::Memory::Paging::kPageSizeBytes);

		const PagePermissions stack_permissions{
			.access = AccessPermissions::ReadWrite,
			.execute = ExecutePermissions::NoExecute,
			.cache = CacheMode::CoherentCached,
			.global = true,
		};

		bool stack_ok = true;
		for (std::size_t i = 0; i < kHigherHalfStackMappedPageCount; i++) {
			const auto page_or = pmm.AllocatePage();
			ROCINANTE_EXPECT_TRUE(ctx, page_or.has_value());
			if (!page_or.has_value()) {
				stack_ok = false;
				break;
			}
			const std::uintptr_t page_virtual = stack_virtual_base + (i * Rocinante::Memory::Paging::kPageSizeBytes);
			const bool mapped = MapPage4KiB(
				&pmm,
				root.value(),
				page_virtual,
				page_or.value(),
				stack_permissions,
				address_bits);
			ROCINANTE_EXPECT_TRUE(ctx, mapped);
			if (!mapped) {
				stack_ok = false;
				break;
			}
		}

		if (stack_ok) {
			g_paging_hw_higher_half_stack_guard_virtual_base = stack_guard_virtual_base;
			g_paging_hw_higher_half_stack_top = stack_virtual_top;
		}
	}

	const auto config_or = Rocinante::Memory::PagingHw::Make4KiBPageWalkerConfig(address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, config_or.has_value());
	Rocinante::Memory::PagingHw::ConfigurePageTableWalker(root.value(), config_or.value());
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();
	Rocinante::Memory::PagingHw::EnablePaging();

	// Mapped-mode access: this should trigger TLBR refill (TLB is invalidated)
	// and then succeed.
	auto* scratch_virtual_u64 = reinterpret_cast<volatile std::uint64_t*>(kScratchVirtualPageBase);
	const auto observed = *scratch_virtual_u64;
	ROCINANTE_EXPECT_EQ_U64(ctx, observed, 0x1122334455667788ull);
	*scratch_virtual_u64 = 0xaabbccddeeff0011ull;
	const auto observed2 = *reinterpret_cast<volatile std::uint64_t*>(kScratchVirtualPageBase);
	ROCINANTE_EXPECT_EQ_U64(ctx, observed2, 0xaabbccddeeff0011ull);

	// Post-paging self-check: mapping a new page must work in mapped mode.
	//
	// This exercises the software paging builder while paging is enabled.
	// The builder must access page-table pages through the physmap.
	static constexpr std::uintptr_t kPostPagingVirtualPageBase =
		kPagingHwScratchVirtualPageBase + (2 * Rocinante::Memory::Paging::kPageSizeBytes);
	static_assert((kPostPagingVirtualPageBase % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	const auto post_paging_page = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, post_paging_page.has_value());
	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(
		&pmm,
		root.value(),
		kPostPagingVirtualPageBase,
		post_paging_page.value(),
		PagePermissions{
			.access = AccessPermissions::ReadWrite,
			.execute = ExecutePermissions::NoExecute,
			.cache = CacheMode::CoherentCached,
			.global = true,
		},
		address_bits));

	auto* post_paging_virtual_u64 = reinterpret_cast<volatile std::uint64_t*>(kPostPagingVirtualPageBase);
	*post_paging_virtual_u64 = 0x0ddc0ffeebadf00dull;
	ROCINANTE_EXPECT_EQ_U64(ctx, *post_paging_virtual_u64, 0x0ddc0ffeebadf00dull);
}

static void Test_PagingHw_PostPaging_MapUnmap_Faults(TestContext* ctx) {
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::PageTableRoot;
	using Rocinante::Memory::Paging::UnmapPage4KiB;

	// This test runs after paging has been enabled.
	// It proves that we can modify mappings in mapped mode, then unmap and observe
	// a paging fault after invalidating the TLB.

	// LoongArch EXCCODE values (Table 21):
	// - 0x2 => PIS: page invalid for store
	static constexpr std::uint64_t kExceptionCodePis = 0x2;

	// Sanity check: paging must be enabled (CRMD.PG=1, CRMD.DA=0).
	static constexpr std::uint32_t kCsrCrmd = 0x0;
	static constexpr std::uint64_t kCrmdPagingEnable = (1ull << 4);
	static constexpr std::uint64_t kCrmdDirectAddressingEnable = (1ull << 3);
	std::uint64_t crmd = 0;
	asm volatile("csrrd %0, %1" : "=r"(crmd) : "i"(kCsrCrmd));
	if ((crmd & kCrmdPagingEnable) == 0 || (crmd & kCrmdDirectAddressingEnable) != 0) {
		ROCINANTE_EXPECT_TRUE(ctx, false);
		return;
	}

	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_root_page_table_physical != 0);
	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_virtual_address_bits != 0);
	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_physical_address_bits != 0);
	if (g_paging_hw_root_page_table_physical == 0 || g_paging_hw_virtual_address_bits == 0 || g_paging_hw_physical_address_bits == 0) {
		ROCINANTE_EXPECT_TRUE(ctx, false);
		return;
	}

	const AddressSpaceBits address_bits{
		.virtual_address_bits = g_paging_hw_virtual_address_bits,
		.physical_address_bits = g_paging_hw_physical_address_bits,
	};

	const PageTableRoot root{.root_physical_address = g_paging_hw_root_page_table_physical};
	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();

	static constexpr std::uintptr_t kPostPagingMapUnmapVirtualPageBase =
		kPagingHwScratchVirtualPageBase + (3 * PhysicalMemoryManager::kPageSizeBytes);
	static_assert((kPostPagingMapUnmapVirtualPageBase % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	const auto page_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, page_or.has_value());
	if (!page_or.has_value()) return;

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(
		&pmm,
		root,
		kPostPagingMapUnmapVirtualPageBase,
		page_or.value(),
		PagePermissions{
			.access = AccessPermissions::ReadWrite,
			.execute = ExecutePermissions::NoExecute,
			.cache = CacheMode::CoherentCached,
			.global = true,
		},
		address_bits));

	// Invalidate the TLB after changing the mapping.
	//
	// LoongArch TLB entries are dual-page: one TLB entry covers an even/odd page
	// pair, with the even page in TLBELO0 and the odd page in TLBELO1.
	// Spec anchor: LoongArch-Vol1-EN.html, Section 7.5.3 (TLBELO0/TLBELO1).
	//
	// This test maps the +3 page. If an earlier TLBR refill populated the TLB
	// entry for the (+2,+3) pair while +3 was unmapped, the cached odd half can
	// still be invalid. Flushing the TLB forces hardware to observe the updated
	// page tables on first access.
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	auto* mapped_u64 = reinterpret_cast<volatile std::uint64_t*>(kPostPagingMapUnmapVirtualPageBase);
	*mapped_u64 = 0x55aa55aa55aa55aaull;
	ROCINANTE_EXPECT_EQ_U64(ctx, *mapped_u64, 0x55aa55aa55aa55aaull);

	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(root, kPostPagingMapUnmapVirtualPageBase, address_bits));
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	ArmExpectedTrap(kExceptionCodePis);
	const std::uint64_t store_value = 0x0123456789abcdefull;
	asm volatile("st.d %0, %1, 0" :: "r"(store_value), "r"(kPostPagingMapUnmapVirtualPageBase) : "memory");
	ROCINANTE_EXPECT_TRUE(ctx, ExpectedTrapObserved());
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapExceptionCode(), kExceptionCodePis);
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapBadVaddr(), kPostPagingMapUnmapVirtualPageBase);
}

static void Test_PagingHw_HigherHalfStack_GuardPageFaults(TestContext* ctx) {
	// This test runs after paging has been enabled.
	// It switches SP to a higher-half mapped stack and then deliberately stores
	// into the unmapped guard page below it.

	// LoongArch EXCCODE values (Table 21):
	// - 0x2 => PIS: page invalid for store
	static constexpr std::uint64_t kExceptionCodePis = 0x2;

	// Sanity check: paging must be enabled (CRMD.PG=1, CRMD.DA=0).
	static constexpr std::uint32_t kCsrCrmd = 0x0;
	static constexpr std::uint64_t kCrmdPagingEnable = (1ull << 4);
	static constexpr std::uint64_t kCrmdDirectAddressingEnable = (1ull << 3);
	std::uint64_t crmd = 0;
	asm volatile("csrrd %0, %1" : "=r"(crmd) : "i"(kCsrCrmd));
	if ((crmd & kCrmdPagingEnable) == 0 || (crmd & kCrmdDirectAddressingEnable) != 0) {
		ROCINANTE_EXPECT_TRUE(ctx, false);
		return;
	}

	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_higher_half_stack_top != 0);
	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_higher_half_stack_guard_virtual_base != 0);
	if (g_paging_hw_higher_half_stack_top == 0 || g_paging_hw_higher_half_stack_guard_virtual_base == 0) {
		ROCINANTE_EXPECT_TRUE(ctx, false);
		return;
	}

	// Store to the first byte of the guard page: this must fault.
	const std::uintptr_t guard_page_probe_address = g_paging_hw_higher_half_stack_guard_virtual_base;
	ArmExpectedTrap(kExceptionCodePis);
	RocinanteTesting_SwitchStackAndStore(
		g_paging_hw_higher_half_stack_top,
		guard_page_probe_address,
		0x0123456789abcdefull);
	ROCINANTE_EXPECT_TRUE(ctx, ExpectedTrapObserved());
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapExceptionCode(), kExceptionCodePis);
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapBadVaddr(), guard_page_probe_address);
}

static void Test_PagingHw_UnmappedAccess_FaultsAndReportsBadV(TestContext* ctx) {
	// This test runs after paging has been enabled.
	// It asserts that an access to an unmapped page faults and reports the fault
	// virtual address in CSR.BADV (exposed via the trap frame).

	// LoongArch EXCCODE values (Table 21):
	// - 0x1 => PIL: page invalid for load
	// - 0x2 => PIS: page invalid for store
	static constexpr std::uint64_t kExceptionCodePil = 0x1;
	static constexpr std::uint64_t kExceptionCodePis = 0x2;

	// Sanity check: paging must be enabled (CRMD.PG=1, CRMD.DA=0).
	// In direct-address mode, this virtual address would be treated as a physical
	// address; the resulting fault mode is platform-dependent and not a paging
	// exception.
	static constexpr std::uint32_t kCsrCrmd = 0x0;
	static constexpr std::uint64_t kCrmdPagingEnable = (1ull << 4);
	static constexpr std::uint64_t kCrmdDirectAddressingEnable = (1ull << 3);
	std::uint64_t crmd = 0;
	asm volatile("csrrd %0, %1" : "=r"(crmd) : "i"(kCsrCrmd));
	if ((crmd & kCrmdPagingEnable) == 0 || (crmd & kCrmdDirectAddressingEnable) != 0) {
		ROCINANTE_EXPECT_TRUE(ctx, false);
		return;
	}

	// Choose a canonical low-half virtual address that is provably unmapped.
	//
	// Suite contract:
	// - The paging smoke test maps a scratch page at kPagingHwScratchVirtualPageBase.
	// - It maps another scratch page at +2 pages.
	// - It does not map the immediately-adjacent page at +1 page.
	static constexpr std::uintptr_t kFaultVirtualAddress =
		kPagingHwScratchVirtualPageBase + Rocinante::Memory::Paging::kPageSizeBytes;
	static_assert((kFaultVirtualAddress % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	// Unmapped load => PIL.
	ArmExpectedTrap(kExceptionCodePil);
	std::uint64_t tmp = 0;
	asm volatile("ld.d %0, %1, 0" : "=r"(tmp) : "r"(kFaultVirtualAddress) : "memory");
	ROCINANTE_EXPECT_TRUE(ctx, ExpectedTrapObserved());
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapExceptionCode(), kExceptionCodePil);
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapBadVaddr(), kFaultVirtualAddress);

	// Unmapped store => PIS.
	ArmExpectedTrap(kExceptionCodePis);
	const std::uint64_t store_value = 0xdeadbeefcafebabeull;
	asm volatile("st.d %0, %1, 0" :: "r"(store_value), "r"(kFaultVirtualAddress) : "memory");
	ROCINANTE_EXPECT_TRUE(ctx, ExpectedTrapObserved());
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapExceptionCode(), kExceptionCodePis);
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapBadVaddr(), kFaultVirtualAddress);
}

} // namespace

extern const TestCase g_test_cases[] = {
	{"CPUCFG.FakeBackend.DecodesWord1", &Test_CPUCFG_FakeBackend_DecodesWord1},
	{"CPUCFG.FakeBackend.CachesWords", &Test_CPUCFG_FakeBackend_CachesWords},
	{"Traps.BREAK.EntersAndReturns", &Test_Traps_BREAK_EntersAndReturns},
	{"Traps.INE.UndefinedInstruction.IsObserved", &Test_Traps_INE_UndefinedInstruction_IsObserved},
	{"Interrupts.TimerIRQ.DeliversAndClears", &Test_Interrupts_TimerIRQ_DeliversAndClears},
	{"Memory.Paging.MapTranslateUnmap", &Test_Paging_MapTranslateUnmap},
	{"Memory.Paging.RespectsVALENAndPALEN", &Test_Paging_RespectsVALENAndPALEN},
	{"Memory.PMM.RespectsReservedKernelAndDTB", &Test_PMM_RespectsReservedKernelAndDTB},
	{"Memory.PagingHw.EnablePaging.TlbRefillSmoke", &Test_PagingHw_EnablePaging_TlbRefillSmoke},
	{"Memory.PagingHw.UnmappedAccess.FaultsAndReportsBadV", &Test_PagingHw_UnmappedAccess_FaultsAndReportsBadV},
	{"Memory.PagingHw.PostPaging.MapUnmap.Faults", &Test_PagingHw_PostPaging_MapUnmap_Faults},
	{"Memory.PagingHw.HigherHalfStack.GuardPageFaults", &Test_PagingHw_HigherHalfStack_GuardPageFaults},
};

extern const std::size_t g_test_case_count = sizeof(g_test_cases) / sizeof(g_test_cases[0]);

} // namespace Rocinante::Testing

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <cstddef>
#include <cstdint>

#include <src/sp/cpucfg.h>
#include <src/trap/trap.h>

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

static std::uint64_t g_paging_fault_observer_invocation_count = 0;
static std::uint64_t g_paging_fault_observer_last_exception_code = 0;
static std::uint64_t g_paging_fault_observer_last_bad_virtual_address = 0;
static std::uint64_t g_paging_fault_observer_last_current_privilege_level = 0;
static Rocinante::Trap::PagingAccessType g_paging_fault_observer_last_access_type = Rocinante::Trap::PagingAccessType::Unknown;

static std::uint16_t g_paging_fault_observer_last_address_space_id = 0;
static std::uint8_t g_paging_fault_observer_last_address_space_id_bits = 0;
static Rocinante::Trap::PagingPgdSelection g_paging_fault_observer_last_pgd_selection =
	Rocinante::Trap::PagingPgdSelection::Unknown;
static std::uint64_t g_paging_fault_observer_last_pgd_base = 0;
static std::uint64_t g_paging_fault_observer_last_pgdl_base = 0;
static std::uint64_t g_paging_fault_observer_last_pgdh_base = 0;

static bool g_paging_fault_pager_did_map = false;
static std::uint64_t g_paging_fault_pager_invocation_count = 0;
static std::uint64_t g_paging_fault_pager_last_bad_virtual_address = 0;
static std::uint64_t g_paging_fault_pager_last_mapped_virtual_page_base = 0;
static std::uint64_t g_paging_fault_pager_last_mapped_physical_page_base = 0;

static std::uint64_t g_paging_hw_nx_fault_invocation_count = 0;
static std::uint64_t g_paging_hw_nx_fault_last_exception_code = 0;
static std::uint64_t g_paging_hw_nx_fault_last_bad_virtual_address = 0;
static Rocinante::Trap::PagingAccessType g_paging_hw_nx_fault_last_access_type =
	Rocinante::Trap::PagingAccessType::Unknown;
static std::uint64_t g_paging_hw_nx_expected_bad_virtual_address_masked = 0;
static std::uint64_t g_paging_hw_nx_resume_exception_return_address = 0;

static Rocinante::Trap::PagingFaultResult PagingFaultObserver_TestNxFetch_RaisesPnx(
	Rocinante::TrapFrame& tf,
	const Rocinante::Trap::PagingFaultEvent& event
) {
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - BADV captures the faulting VA for paging exceptions, but for LA64 only
	//   bits [VALEN-1:13] are architecturally recorded.
	static constexpr std::uint64_t kBadvLowBitsMask = (1ull << 13) - 1;

	const std::uint64_t badv_masked = event.bad_virtual_address & ~kBadvLowBitsMask;
	if (badv_masked != g_paging_hw_nx_expected_bad_virtual_address_masked) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	g_paging_hw_nx_fault_invocation_count++;
	g_paging_hw_nx_fault_last_exception_code = event.exception_code;
	g_paging_hw_nx_fault_last_bad_virtual_address = event.bad_virtual_address;
	g_paging_hw_nx_fault_last_access_type = event.access_type;

	// Preferred resume policy: jump to a test-provided mapped return site.
	//
	// Rationale:
	// - For instruction-fetch faults (e.g. PNX), skipping +4 does not escape the
	//   NX mapping.
	// - Using $ra (r1) is fragile if the trapframe doesn't reliably preserve it
	//   across all exception types/emulator behaviors.
	if (g_paging_hw_nx_resume_exception_return_address != 0) {
		tf.exception_return_address = g_paging_hw_nx_resume_exception_return_address;
		return Rocinante::Trap::PagingFaultResult::Handled;
	}

	// Resume execution by returning to the saved return address ($ra = r1).
	//
	// Rationale:
	// - Instruction-fetch faults (e.g. PNX) report an ERA that points into the
	//   faulting page. Advancing ERA by +4 would not escape the NX mapping.
	// - The indirect call that triggers the fetch sets $ra to a mapped return site.
	//
	// TrapFrame contract: general_purpose_registers[i] corresponds to GPR r{i}.
	static constexpr std::size_t kRegisterRa = 1;
	tf.exception_return_address = tf.general_purpose_registers[kRegisterRa];
	return Rocinante::Trap::PagingFaultResult::Handled;
}

static Rocinante::Trap::PagingFaultResult PagingFaultObserver_TestProbe(
	Rocinante::TrapFrame& tf,
	const Rocinante::Trap::PagingFaultEvent& event
) {
	// Record observation for the test to assert after returning.
	g_paging_fault_observer_invocation_count++;
	g_paging_fault_observer_last_exception_code = event.exception_code;
	g_paging_fault_observer_last_bad_virtual_address = event.bad_virtual_address;
	g_paging_fault_observer_last_current_privilege_level = event.current_privilege_level;
	g_paging_fault_observer_last_access_type = event.access_type;
	g_paging_fault_observer_last_address_space_id = event.address_space_id;
	g_paging_fault_observer_last_address_space_id_bits = event.address_space_id_bits;
	g_paging_fault_observer_last_pgd_selection = event.pgd_selection;
	g_paging_fault_observer_last_pgd_base = event.pgd_base;
	g_paging_fault_observer_last_pgdl_base = event.pgdl_base;
	g_paging_fault_observer_last_pgdh_base = event.pgdh_base;

	// Handle by skipping the faulting instruction.
	// LoongArch instructions are 32-bit.
	static constexpr std::uint64_t kInstructionSizeBytes = 4;
	tf.exception_return_address += kInstructionSizeBytes;
	return Rocinante::Trap::PagingFaultResult::Handled;
}

static Rocinante::Trap::PagingFaultResult PagingFaultObserver_TestPagerMapAndRetry(
	Rocinante::TrapFrame& tf,
	const Rocinante::Trap::PagingFaultEvent& event
) {
	(void)tf;

	g_paging_fault_pager_invocation_count++;
	g_paging_fault_pager_last_bad_virtual_address = event.bad_virtual_address;

	if (g_paging_hw_virtual_address_bits == 0 || g_paging_hw_physical_address_bits == 0) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Test pager policy: handle one canonical scratch-adjacent missing page by mapping it.
	static constexpr std::uintptr_t kExpectedFaultVirtualAddress =
		kPagingHwScratchVirtualPageBase + Rocinante::Memory::Paging::kPageSizeBytes;
	static_assert((kExpectedFaultVirtualAddress % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	if (event.bad_virtual_address != kExpectedFaultVirtualAddress) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}
	if (g_paging_fault_pager_did_map) {
		// Avoid infinite recursion if something goes wrong.
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Map the missing page into the active root for this address.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Vol.1 Section 7.5.7 (PGD), Table 41: CSR.PGD provides the effective root
	//   page-directory Base corresponding to CSR.BADV in the current fault context.
	const Rocinante::Memory::Paging::PageTableRoot root{
		.root_physical_address = static_cast<std::uintptr_t>(event.pgd_base),
	};

	const Rocinante::Memory::Paging::AddressSpaceBits address_bits{
		.virtual_address_bits = g_paging_hw_virtual_address_bits,
		.physical_address_bits = g_paging_hw_physical_address_bits,
	};

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	const auto page_or = pmm.AllocatePage();
	if (!page_or.has_value()) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	static constexpr Rocinante::Memory::Paging::PagePermissions kPermissions{
		.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
		.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
		.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
		.global = true,
	};

	const bool mapped = Rocinante::Memory::Paging::MapPage4KiB(
		&pmm,
		root,
		kExpectedFaultVirtualAddress,
		page_or.value(),
		kPermissions,
		address_bits);
	if (!mapped) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Ensure the re-executed instruction observes the updated page tables.
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	g_paging_fault_pager_did_map = true;
	g_paging_fault_pager_last_mapped_virtual_page_base = kExpectedFaultVirtualAddress;
	g_paging_fault_pager_last_mapped_physical_page_base = page_or.value();

	// Important: do NOT advance ERA. Returning Handled should retry the instruction.
	return Rocinante::Trap::PagingFaultResult::Handled;
}

extern "C" void RocinanteTesting_SwitchStackAndStore(
	std::uintptr_t new_stack_pointer,
	std::uintptr_t store_address,
	std::uint64_t store_value);

extern "C" void RocinanteTesting_StoreAndReturn(
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

	.globl RocinanteTesting_StoreAndReturn
	.type RocinanteTesting_StoreAndReturn, @function
	.p2align 2
RocinanteTesting_StoreAndReturn:
	st.d   $a1, $a0, 0
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

	// Expected free pages: total usable (16) minus reserved (2) minus kernel (4) minus DTB (1)
	// minus PMM bitmap storage (at least 1 page) = 8.
	static constexpr std::size_t kExpectedTotalPages = 16;
	static constexpr std::size_t kExpectedFreePages = 8;
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

	// Free pages: total minus bitmap-storage page (always at least 1 page) minus
	// the zero page reservation.
	static constexpr std::size_t kExpectedFreePages = kExpectedTotalPages - 1 - 1;
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
	// - Zero page: explicitly reserved by PMM policy (since kernel is not at physical 0)
	static constexpr std::size_t kExpectedTotalPages = 1024;
	static constexpr std::size_t kExpectedFreePages = 1024 - 256 - 64 - 1 - 1;
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.TotalPages(), kExpectedTotalPages);
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), kExpectedFreePages);
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

static void Test_Paging_Physmap_MapsRootPageTableAndAttributes(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapRange4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::PageTablePage;
	using Rocinante::Memory::Paging::PageTableRoot;
	using Rocinante::Memory::Paging::Translate;
	namespace PteBits = Rocinante::Memory::Paging::PteBits;

	// Regression guard for paging bring-up:
	// The paging builder/walker must be able to access page-table pages via the
	// physmap once paging is enabled.

	auto& cpucfg = Rocinante::GetCPUCFG();
	const AddressSpaceBits address_bits{
		.virtual_address_bits = static_cast<std::uint8_t>(cpucfg.VirtualAddressBits()),
		.physical_address_bits = static_cast<std::uint8_t>(cpucfg.PhysicalAddressBits()),
	};

	// Keep the pool away from the kernel/DTB low region.
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

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	ROCINANTE_EXPECT_TRUE(ctx, pmm.InitializeFromBootMemoryMap(
		map,
		kernel_physical_base,
		kernel_physical_end,
		0,
		0));

	const auto root_or = AllocateRootPageTable(&pmm);
	ROCINANTE_EXPECT_TRUE(ctx, root_or.has_value());
	if (!root_or.has_value()) return;
	const PageTableRoot root = root_or.value();

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
		root,
		physmap_virtual_base,
		kUsableBase,
		kUsableSizeBytes,
		physmap_permissions,
		address_bits));

	// The root page-table page must be allocated from the PMM pool.
	ROCINANTE_EXPECT_TRUE(ctx, root.root_physical_address >= kUsableBase);
	ROCINANTE_EXPECT_TRUE(ctx, root.root_physical_address < (kUsableBase + kUsableSizeBytes));

	const std::uintptr_t physmap_root_virtual =
		physmap_virtual_base + (root.root_physical_address - kUsableBase);

	const auto translated = Translate(root, physmap_root_virtual, address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, translated.has_value());
	if (translated.has_value()) {
		ROCINANTE_EXPECT_EQ_U64(ctx, translated.value(), root.root_physical_address);
	}

	// Read the physmap leaf PTE and validate cache + NX.
	const auto ReadLeafPteEntry_Assuming4Level4KiB =
		[&](std::uintptr_t probe_va) -> Rocinante::Optional<std::uint64_t> {
			constexpr std::size_t kIndexMask =
				(1u << Rocinante::Memory::Paging::kIndexBitsPerLevel) - 1u;
			constexpr std::size_t kShiftPt = Rocinante::Memory::Paging::kPageShiftBits;
			constexpr std::size_t kShiftDirl = kShiftPt + Rocinante::Memory::Paging::kIndexBitsPerLevel;
			constexpr std::size_t kShiftDir2 = kShiftDirl + Rocinante::Memory::Paging::kIndexBitsPerLevel;
			constexpr std::size_t kShiftDir3 = kShiftDir2 + Rocinante::Memory::Paging::kIndexBitsPerLevel;

			const std::size_t idx_dir3 = static_cast<std::size_t>((probe_va >> kShiftDir3) & kIndexMask);
			const std::size_t idx_dir2 = static_cast<std::size_t>((probe_va >> kShiftDir2) & kIndexMask);
			const std::size_t idx_dirl = static_cast<std::size_t>((probe_va >> kShiftDirl) & kIndexMask);
			const std::size_t idx_pt = static_cast<std::size_t>((probe_va >> kShiftPt) & kIndexMask);

			auto* dir3 = reinterpret_cast<PageTablePage*>(root.root_physical_address);
			if (!dir3) return Rocinante::nullopt;

			const auto EntryIsWalkable = [](std::uint64_t entry) {
				return (entry & (PteBits::kValid | PteBits::kPresent)) == (PteBits::kValid | PteBits::kPresent);
			};
			const auto EntryBase4K = [](std::uint64_t entry) {
				return static_cast<std::uintptr_t>(
					entry & ~static_cast<std::uint64_t>(Rocinante::Memory::Paging::kPageOffsetMask)
				);
			};

			const std::uint64_t e3 = dir3->entries[idx_dir3];
			if (!EntryIsWalkable(e3)) return Rocinante::nullopt;
			auto* dir2 = reinterpret_cast<PageTablePage*>(EntryBase4K(e3));
			if (!dir2) return Rocinante::nullopt;

			const std::uint64_t e2 = dir2->entries[idx_dir2];
			if (!EntryIsWalkable(e2)) return Rocinante::nullopt;
			auto* dirl = reinterpret_cast<PageTablePage*>(EntryBase4K(e2));
			if (!dirl) return Rocinante::nullopt;

			const std::uint64_t e1 = dirl->entries[idx_dirl];
			if (!EntryIsWalkable(e1)) return Rocinante::nullopt;
			auto* pt = reinterpret_cast<PageTablePage*>(EntryBase4K(e1));
			if (!pt) return Rocinante::nullopt;

			return Rocinante::Optional<std::uint64_t>(pt->entries[idx_pt]);
		};

	const auto pte_or = ReadLeafPteEntry_Assuming4Level4KiB(physmap_root_virtual);
	ROCINANTE_EXPECT_TRUE(ctx, pte_or.has_value());
	if (!pte_or.has_value()) return;
	const std::uint64_t pte = pte_or.value();

	const std::uint64_t cache_field = (pte & PteBits::kCacheMask) >> PteBits::kCacheShift;
	const bool nx = (pte & PteBits::kNoExecute) != 0;
	ROCINANTE_EXPECT_TRUE(ctx, nx);
	ROCINANTE_EXPECT_EQ_U64(ctx, cache_field, static_cast<std::uint64_t>(CacheMode::CoherentCached));
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

static void Test_PagingHw_PagingFaultObserver_DispatchesAndCanHandle(TestContext* ctx) {
	// This test runs after paging has been enabled.
	// It installs a paging-fault observer and asserts that:
	// - the observer is invoked for a paging exception, and
	// - returning Handled can resume execution (by advancing ERA).

	// LoongArch EXCCODE values (Table 21):
	// - 0x1 => PIL: page invalid for load
	static constexpr std::uint64_t kExceptionCodePil = 0x1;

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

	// Use the same provably-unmapped scratch-adjacent page as the unmapped-access test.
	static constexpr std::uintptr_t kFaultVirtualAddress =
		kPagingHwScratchVirtualPageBase + Rocinante::Memory::Paging::kPageSizeBytes;
	static_assert((kFaultVirtualAddress % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	// Reset observations and install observer.
	g_paging_fault_observer_invocation_count = 0;
	g_paging_fault_observer_last_exception_code = 0;
	g_paging_fault_observer_last_bad_virtual_address = 0;
	g_paging_fault_observer_last_current_privilege_level = 0;
	g_paging_fault_observer_last_access_type = Rocinante::Trap::PagingAccessType::Unknown;
	g_paging_fault_observer_last_address_space_id = 0;
	g_paging_fault_observer_last_address_space_id_bits = 0;
	g_paging_fault_observer_last_pgd_selection = Rocinante::Trap::PagingPgdSelection::Unknown;
	g_paging_fault_observer_last_pgd_base = 0;
	g_paging_fault_observer_last_pgdl_base = 0;
	g_paging_fault_observer_last_pgdh_base = 0;

	Rocinante::Trap::SetPagingFaultObserver(&PagingFaultObserver_TestProbe);
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	// Trigger an unmapped load. The observer handles the fault by skipping the instruction.
	std::uint64_t tmp = 0;
	asm volatile("ld.d %0, %1, 0" : "=r"(tmp) : "r"(kFaultVirtualAddress) : "memory");

	// Always clear the observer so later tests keep their existing behavior.
	Rocinante::Trap::SetPagingFaultObserver(nullptr);

	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_invocation_count, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_exception_code, kExceptionCodePil);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_bad_virtual_address, kFaultVirtualAddress);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_current_privilege_level, 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(g_paging_fault_observer_last_access_type),
		static_cast<std::uint64_t>(Rocinante::Trap::PagingAccessType::Load));

	// Address-space and PGD identity should be captured.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Vol.1 Section 7.5.4 (ASID), Table 38.
	// - Vol.1 Section 7.5.7 (PGD), Table 41.
	static constexpr std::uint32_t kCsrAsid = 0x18;
	static constexpr std::uint32_t kCsrPgdl = 0x19;
	static constexpr std::uint32_t kCsrPgdh = 0x1a;
	static constexpr std::uint32_t kCsrPgd = 0x1b;
	static constexpr std::uint64_t kAsidMask = 0x3ff;
	static constexpr std::uint64_t kAsidBitsShift = 16;
	static constexpr std::uint64_t kAsidBitsMask = 0xff;
	static constexpr std::uint64_t kPgdBaseMask = 0xfffffffffffff000ull;

	std::uint64_t asid_csr = 0;
	std::uint64_t pgdl_csr = 0;
	std::uint64_t pgdh_csr = 0;
	std::uint64_t pgd_csr = 0;
	asm volatile("csrrd %0, %1" : "=r"(asid_csr) : "i"(kCsrAsid));
	asm volatile("csrrd %0, %1" : "=r"(pgdl_csr) : "i"(kCsrPgdl));
	asm volatile("csrrd %0, %1" : "=r"(pgdh_csr) : "i"(kCsrPgdh));
	asm volatile("csrrd %0, %1" : "=r"(pgd_csr) : "i"(kCsrPgd));

	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_address_space_id,
		static_cast<std::uint64_t>(asid_csr & kAsidMask));
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_address_space_id_bits,
		static_cast<std::uint64_t>((asid_csr >> kAsidBitsShift) & kAsidBitsMask));
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_pgd_base, (pgd_csr & kPgdBaseMask));
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_pgdl_base, (pgdl_csr & kPgdBaseMask));
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_observer_last_pgdh_base, (pgdh_csr & kPgdBaseMask));
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(g_paging_fault_observer_last_pgd_selection),
		static_cast<std::uint64_t>(Rocinante::Trap::PagingPgdSelection::LowHalf));
}

static void Test_PagingHw_PagingFaultObserver_MapsAndRetries(TestContext* ctx) {
	// This test runs after paging has been enabled.
	// It installs a paging-fault observer that maps the missing page and proves
	// we can recover by retrying the faulting instruction (no ERA adjustment).

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

	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_virtual_address_bits != 0);
	ROCINANTE_EXPECT_TRUE(ctx, g_paging_hw_physical_address_bits != 0);
	if (g_paging_hw_virtual_address_bits == 0 || g_paging_hw_physical_address_bits == 0) {
		ROCINANTE_EXPECT_TRUE(ctx, false);
		return;
	}

	// Use the same provably-unmapped scratch-adjacent page as the unmapped-access test.
	static constexpr std::uintptr_t kFaultVirtualAddress =
		kPagingHwScratchVirtualPageBase + Rocinante::Memory::Paging::kPageSizeBytes;
	static_assert((kFaultVirtualAddress % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	// Reset pager observations and install observer.
	g_paging_fault_pager_did_map = false;
	g_paging_fault_pager_invocation_count = 0;
	g_paging_fault_pager_last_bad_virtual_address = 0;
	g_paging_fault_pager_last_mapped_virtual_page_base = 0;
	g_paging_fault_pager_last_mapped_physical_page_base = 0;

	Rocinante::Trap::SetPagingFaultObserver(&PagingFaultObserver_TestPagerMapAndRetry);
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	// Trigger an unmapped store. The observer handles the fault by mapping the page,
	// then the store is retried and must succeed.
	const std::uint64_t store_value = 0x0123456789abcdefull;
	asm volatile("st.d %0, %1, 0" :: "r"(store_value), "r"(kFaultVirtualAddress) : "memory");

	// Always clear the observer so later tests keep their existing behavior.
	Rocinante::Trap::SetPagingFaultObserver(nullptr);

	volatile std::uint64_t* const p = reinterpret_cast<volatile std::uint64_t*>(kFaultVirtualAddress);
	ROCINANTE_EXPECT_EQ_U64(ctx, *p, store_value);

	ROCINANTE_EXPECT_TRUE(ctx, g_paging_fault_pager_did_map);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_pager_invocation_count, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_pager_last_bad_virtual_address, kFaultVirtualAddress);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_fault_pager_last_mapped_virtual_page_base, kFaultVirtualAddress);
	ROCINANTE_EXPECT_TRUE(ctx, g_paging_fault_pager_last_mapped_physical_page_base != 0);
}

static void Test_PagingHw_ReadOnlyStore_RaisesPme(TestContext* ctx) {
	// This test runs after paging has been enabled.
	// It maps a page with D=0 (no-dirty / no-write) and asserts that a store
	// triggers PME (EXCCODE=0x4) and reports the fault VA in BADV.

	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::PageTableRoot;

	// LoongArch EXCCODE values (Table 21):
	// - 0x4 => PME: page modification exception
	static constexpr std::uint64_t kExceptionCodePme = 0x4;

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

	// Choose a scratch-adjacent page not otherwise used by the suite.
	static constexpr std::uintptr_t kReadOnlyVirtualPageBase =
		kPagingHwScratchVirtualPageBase + (4 * PhysicalMemoryManager::kPageSizeBytes);
	static_assert((kReadOnlyVirtualPageBase % PhysicalMemoryManager::kPageSizeBytes) == 0);

	const auto page_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, page_or.has_value());
	if (!page_or.has_value()) return;

	// Initialize the physical page through the physmap so a mapped-mode load can verify it.
	const std::uintptr_t physmap_virtual =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(page_or.value(), address_bits.virtual_address_bits);
	auto* physmap_u64 = reinterpret_cast<volatile std::uint64_t*>(physmap_virtual);
	*physmap_u64 = 0x1122334455667788ull;

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(
		&pmm,
		root,
		kReadOnlyVirtualPageBase,
		page_or.value(),
		PagePermissions{
			.access = AccessPermissions::ReadOnly,
			.execute = ExecutePermissions::NoExecute,
			.cache = CacheMode::CoherentCached,
			.global = true,
		},
		address_bits));

	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	// Mapped-mode load must succeed.
	auto* readonly_u64 = reinterpret_cast<volatile std::uint64_t*>(kReadOnlyVirtualPageBase);
	ROCINANTE_EXPECT_EQ_U64(ctx, *readonly_u64, 0x1122334455667788ull);

	// Store to a valid page with D=0 must raise PME (spec: 5.4.4 pseudocode + Table 21).
	ArmExpectedTrap(kExceptionCodePme);
	const std::uint64_t store_value = 0xaabbccddeeff0011ull;
	asm volatile("st.d %0, %1, 0" :: "r"(store_value), "r"(kReadOnlyVirtualPageBase) : "memory");
	ROCINANTE_EXPECT_TRUE(ctx, ExpectedTrapObserved());
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapExceptionCode(), kExceptionCodePme);
	ROCINANTE_EXPECT_EQ_U64(ctx, ExpectedTrapBadVaddr(), kReadOnlyVirtualPageBase);
}

static void Test_PagingHw_NonExecutableFetch_RaisesPnx(TestContext* ctx) {
	// This test runs after paging has been enabled.
	// It maps a known executable code page at a scratch VA with NX=1 and asserts
	// that an instruction fetch through that alias triggers PNX (EXCCODE=0x6).

	// Spec anchor (LoongArch-Vol1-EN.html, CPUCFG word 0x1):
	// - Bit 22 (EP): "1 indicates support for page attribute of 'Execution Protection'".
	// If EP=0, NX may be ignored/cleared by the CPU (or emulator), so this test
	// is not applicable.
	if (!Rocinante::GetCPUCFG().SupportsExecProtection()) {
		// Output a warning instead of a failure since this is a platform limitation, not a test failure.
		Rocinante::Testing::Warn(ctx, __FILE__, __LINE__,
			"CPU does not support execution protection (CPUCFG.EP=0); skipping non-executable fetch test");
		return;
	}

	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::PageTableRoot;
	using Rocinante::Memory::Paging::Translate;
	using Rocinante::Memory::Paging::UnmapPage4KiB;
	namespace PteBits = Rocinante::Memory::Paging::PteBits;

	// LoongArch EXCCODE values (Table 21):
	// - 0x6 => PNX: page non-executable exception
	static constexpr std::uint64_t kExceptionCodePnx = 0x6;

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

	// Choose a scratch-adjacent page not otherwise used by the suite.
	// Use an even page in a dual-page (8 KiB) pair to avoid BADV low-bit ambiguity.
	static constexpr std::uintptr_t kNxAliasVirtualPageBase =
		kPagingHwScratchVirtualPageBase + (6 * PhysicalMemoryManager::kPageSizeBytes);
	static_assert((kNxAliasVirtualPageBase % (2 * PhysicalMemoryManager::kPageSizeBytes)) == 0);

	const std::uintptr_t target_virtual = reinterpret_cast<std::uintptr_t>(&RocinanteTesting_StoreAndReturn);
	const std::uintptr_t target_virtual_page_base =
		target_virtual & ~(PhysicalMemoryManager::kPageSizeBytes - 1);
	const std::uintptr_t target_offset = target_virtual - target_virtual_page_base;
	ROCINANTE_EXPECT_TRUE(ctx, target_offset < PhysicalMemoryManager::kPageSizeBytes);

	const auto target_physical_page0_or = Translate(root, target_virtual_page_base, address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, target_physical_page0_or.has_value());
	if (!target_physical_page0_or.has_value()) return;

	const std::uintptr_t target_virtual_page_base_plus1 = target_virtual_page_base + PhysicalMemoryManager::kPageSizeBytes;
	const auto target_physical_page1_or = Translate(root, target_virtual_page_base_plus1, address_bits);

	const std::uintptr_t alias_target_virtual = kNxAliasVirtualPageBase + target_offset;

	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Page non-executable exception: fetch finds a match in TLB with V=1, PLV legal, NX=1.
	static constexpr PagePermissions kNxPermissions{
		.access = AccessPermissions::ReadOnly,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	const bool mapped0 = MapPage4KiB(
		&pmm,
		root,
		kNxAliasVirtualPageBase,
		static_cast<std::uintptr_t>(target_physical_page0_or.value()),
		kNxPermissions,
		address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, mapped0);
	if (!mapped0) return;

	bool mapped1 = false;
	if (target_physical_page1_or.has_value()) {
		mapped1 = MapPage4KiB(
			&pmm,
			root,
			kNxAliasVirtualPageBase + PhysicalMemoryManager::kPageSizeBytes,
			static_cast<std::uintptr_t>(target_physical_page1_or.value()),
			kNxPermissions,
			address_bits);
		ROCINANTE_EXPECT_TRUE(ctx, mapped1);
	}


	// Diagnostic: read back the raw leaf PTE from memory (via physmap).
	// This localizes whether NX is missing in the page table entry itself, vs.
	// being dropped during hardware page-walk refill/TLBFILL.
	{
		using Rocinante::Memory::Paging::PageTablePage;
		const auto ReadLeafPteEntry_UsingPhysmap_Assuming4Level4KiB =
			[&](std::uintptr_t probe_va) -> Rocinante::Optional<std::uint64_t> {
				constexpr std::size_t kIndexMask =
					(1u << Rocinante::Memory::Paging::kIndexBitsPerLevel) - 1u;
				constexpr std::size_t kShiftPt = Rocinante::Memory::Paging::kPageShiftBits;
				constexpr std::size_t kShiftDirl = kShiftPt + Rocinante::Memory::Paging::kIndexBitsPerLevel;
				constexpr std::size_t kShiftDir2 = kShiftDirl + Rocinante::Memory::Paging::kIndexBitsPerLevel;
				constexpr std::size_t kShiftDir3 = kShiftDir2 + Rocinante::Memory::Paging::kIndexBitsPerLevel;

				const std::size_t idx_dir3 = static_cast<std::size_t>((probe_va >> kShiftDir3) & kIndexMask);
				const std::size_t idx_dir2 = static_cast<std::size_t>((probe_va >> kShiftDir2) & kIndexMask);
				const std::size_t idx_dirl = static_cast<std::size_t>((probe_va >> kShiftDirl) & kIndexMask);
				const std::size_t idx_pt = static_cast<std::size_t>((probe_va >> kShiftPt) & kIndexMask);

				const auto PhysToPhysmap = [&](std::uintptr_t physical) -> std::uintptr_t {
					return Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(
						physical,
						address_bits.virtual_address_bits);
				};

				auto* dir3 = reinterpret_cast<PageTablePage*>(PhysToPhysmap(root.root_physical_address));
				if (!dir3) return Rocinante::nullopt;

				const auto EntryIsWalkable = [](std::uint64_t entry) {
					return (entry & (PteBits::kValid | PteBits::kPresent)) == (PteBits::kValid | PteBits::kPresent);
				};
				const auto EntryBase4K = [](std::uint64_t entry) {
					return static_cast<std::uintptr_t>(
						entry & ~static_cast<std::uint64_t>(Rocinante::Memory::Paging::kPageOffsetMask)
					);
				};

				const std::uint64_t e3 = dir3->entries[idx_dir3];
				if (!EntryIsWalkable(e3)) return Rocinante::nullopt;
				auto* dir2 = reinterpret_cast<PageTablePage*>(PhysToPhysmap(EntryBase4K(e3)));
				if (!dir2) return Rocinante::nullopt;

				const std::uint64_t e2 = dir2->entries[idx_dir2];
				if (!EntryIsWalkable(e2)) return Rocinante::nullopt;
				auto* dirl = reinterpret_cast<PageTablePage*>(PhysToPhysmap(EntryBase4K(e2)));
				if (!dirl) return Rocinante::nullopt;

				const std::uint64_t e1 = dirl->entries[idx_dirl];
				if (!EntryIsWalkable(e1)) return Rocinante::nullopt;
				auto* pt = reinterpret_cast<PageTablePage*>(PhysToPhysmap(EntryBase4K(e1)));
				if (!pt) return Rocinante::nullopt;

				return Rocinante::Optional<std::uint64_t>(pt->entries[idx_pt]);
			};

		const auto pte_or = ReadLeafPteEntry_UsingPhysmap_Assuming4Level4KiB(alias_target_virtual);
		ROCINANTE_EXPECT_TRUE(ctx, pte_or.has_value());
		if (pte_or.has_value()) {
			const std::uint64_t pte = pte_or.value();
			ROCINANTE_EXPECT_TRUE(ctx, (pte & PteBits::kNoExecute) != 0);

			const std::uint64_t physical_mask =
				(address_bits.physical_address_bits >= 64)
					? ~0ull
					: ((1ull << address_bits.physical_address_bits) - 1ull);
			const std::uint64_t pte_phys_base =
				(pte & physical_mask) & ~static_cast<std::uint64_t>(Rocinante::Memory::Paging::kPageOffsetMask);
			ROCINANTE_EXPECT_EQ_U64(ctx, pte_phys_base, static_cast<std::uint64_t>(target_physical_page0_or.value()));
		}
	}

	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();

	// Reset observations.
	g_paging_hw_nx_fault_invocation_count = 0;
	g_paging_hw_nx_fault_last_exception_code = 0;
	g_paging_hw_nx_fault_last_bad_virtual_address = 0;
	g_paging_hw_nx_fault_last_access_type = Rocinante::Trap::PagingAccessType::Unknown;
	g_paging_hw_nx_resume_exception_return_address = 0;

	// Spec anchor (LoongArch-Vol1-EN.html):
	// - For LA64, BADV records bits [VALEN-1:13] of the faulting VA for paging exceptions.
	static constexpr std::uint64_t kBadvLowBitsMask = (1ull << 13) - 1;
	g_paging_hw_nx_expected_bad_virtual_address_masked =
		static_cast<std::uint64_t>(alias_target_virtual) & ~kBadvLowBitsMask;

	// Install observer immediately before triggering the NX fetch.
	Rocinante::Trap::SetPagingFaultObserver(&PagingFaultObserver_TestNxFetch_RaisesPnx);

	// Trigger an instruction fetch through the NX alias.
	//
	// If NX is incorrectly ignored, the alias will execute this stub and perform
	// the store, then return normally. If NX is enforced, we should observe a PNX
	// exception before the first instruction executes.
	volatile std::uint64_t observed_store_value = 0;
	static constexpr std::uint64_t kExpectedStoreValue = 0xfeedfacecafebeefull;

	using Fn = void (*)(std::uintptr_t, std::uint64_t);
	volatile Fn fn = reinterpret_cast<Fn>(alias_target_virtual);
	// GNU extension: take the address of a local label to use as a known-good
	// exception return site when the NX fetch faults.
	g_paging_hw_nx_resume_exception_return_address = reinterpret_cast<std::uint64_t>(&&nx_resume);
	fn(reinterpret_cast<std::uintptr_t>(&observed_store_value), kExpectedStoreValue);

nx_resume:
	g_paging_hw_nx_resume_exception_return_address = 0;

	// Always clear the observer so later tests keep their existing behavior.
	Rocinante::Trap::SetPagingFaultObserver(nullptr);

	// If NX is enforced, we should observe a PNX fault and the stub should NOT run.
	if (g_paging_hw_nx_fault_invocation_count == 0) {
		// EP=1 but no PNX observed. This is a hard failure: it means NX is either
		// not being enforced by the platform/emulator, or the refill/fill pipeline
		// is not preserving the NX attribute.
		//
		// In fact, we're hitting this failure on QEMU LoongArch, which reports EP=1
		// I've confirmed this to be a QEMU bug. (NX bit is not respected in PTEs)
		// Report: https://gitlab.com/qemu-project/qemu/-/issues/3319
		// I've submitted a patch upstream,
		// in the meantime we'll just be using my patched QEMU build for testing
		// Source is available at: https://github.com/rail5/qemu
		Rocinante::Testing::Fail(ctx, __FILE__, __LINE__,
			"EP=1 but no PNX observed for NX-mapped alias fetch");
	}

	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_hw_nx_fault_invocation_count, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, g_paging_hw_nx_fault_last_exception_code, kExceptionCodePnx);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(g_paging_hw_nx_fault_last_access_type),
		static_cast<std::uint64_t>(Rocinante::Trap::PagingAccessType::Fetch));

	// BADV comparison: check the architecturally recorded portion.
	ROCINANTE_EXPECT_EQ_U64(ctx,
		(g_paging_hw_nx_fault_last_bad_virtual_address & ~kBadvLowBitsMask),
		g_paging_hw_nx_expected_bad_virtual_address_masked);

	ROCINANTE_EXPECT_EQ_U64(ctx, observed_store_value, 0);

	// Clean up the mapping to avoid TLB/page-table state leaking into later tests.
	(void)UnmapPage4KiB(root, kNxAliasVirtualPageBase, address_bits);
	if (mapped1) {
		(void)UnmapPage4KiB(root, kNxAliasVirtualPageBase + PhysicalMemoryManager::kPageSizeBytes, address_bits);
	}
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();
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
	{"Memory.Paging.Physmap.MapsRootAndAttributes", &Test_Paging_Physmap_MapsRootPageTableAndAttributes},
	{"Memory.PMM.RespectsReservedKernelAndDTB", &Test_PMM_RespectsReservedKernelAndDTB},
	{"Memory.PMM.BitmapPlacement.DoesNotClobberReserved", &Test_PMM_DoesNotClobberReservedDuringBitmapPlacement},
	{"Memory.PMM.ClampsTrackedRangeToPALEN", &Test_PMM_ClampsTrackedRangeToPALEN},
	{"Memory.PMM.BitmapPlacement.RespectsPALEN", &Test_PMM_BitmapPlacement_RespectsPALEN},
	{"Memory.PMM.Initialize.SingleUsableRegionContainingKernelAndDTB", &Test_PMM_Initialize_SingleUsableRegionContainingKernelAndDTB},
	{"Memory.PagingHw.EnablePaging.TlbRefillSmoke", &Test_PagingHw_EnablePaging_TlbRefillSmoke},
	{"Memory.PagingHw.UnmappedAccess.FaultsAndReportsBadV", &Test_PagingHw_UnmappedAccess_FaultsAndReportsBadV},
	{"Memory.PagingHw.PagingFaultObserver.DispatchesAndCanHandle", &Test_PagingHw_PagingFaultObserver_DispatchesAndCanHandle},
	{"Memory.PagingHw.PagingFaultObserver.MapsAndRetries", &Test_PagingHw_PagingFaultObserver_MapsAndRetries},
	{"Memory.PagingHw.ReadOnlyStore.RaisesPME", &Test_PagingHw_ReadOnlyStore_RaisesPme},
	{"Memory.PagingHw.NonExecutableFetch.RaisesPNX", &Test_PagingHw_NonExecutableFetch_RaisesPnx},
	{"Memory.PagingHw.PostPaging.MapUnmap.Faults", &Test_PagingHw_PostPaging_MapUnmap_Faults},
	{"Memory.PagingHw.HigherHalfStack.GuardPageFaults", &Test_PagingHw_HigherHalfStack_GuardPageFaults},
};

extern const std::size_t g_test_case_count = sizeof(g_test_cases) / sizeof(g_test_cases[0]);

} // namespace Rocinante::Testing

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpucfg.h>
#include <src/trap/trap.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/address_space.h>
#include <src/memory/pmm.h>
#include <src/memory/paging.h>
#include <src/memory/paging_hw.h>
#include <src/memory/virtual_layout.h>

#include <cstddef>
#include <cstdint>

extern "C" char _start;
extern "C" char _end;

namespace Rocinante::Testing {

namespace {

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

static void Test_PagingHw_AddressSpaces_SwitchPgdlChangesTranslation(TestContext* ctx) {
	using Rocinante::Memory::AddressSpace;
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::PagePermissions;

	// This is an end-to-end test for the most primitive form of “multiple address spaces”:
	// switching CSR.PGDL (low-half root) + CSR.ASID changes the translation for a low-half VA.
	//
	// Why this is useful even without a scheduler:
	// - It validates the hardware mechanism for address-space switching.
	// - It lets us build VMM/user-space abstractions without committing to threads.
	//
	// Requirements:
	// - Paging must already be enabled by the earlier smoke test.
	// - The kernel image is executing from a low-half identity mapping in tests,
	//   so BOTH roots must map the kernel image + MMIO + physmap.
	if (g_paging_hw_virtual_address_bits == 0 || g_paging_hw_physical_address_bits == 0) {
		Rocinante::Testing::Fail(ctx, __FILE__, __LINE__, "paging not enabled / address bits not initialized");
		return;
	}

	const AddressSpaceBits address_bits{
		.virtual_address_bits = g_paging_hw_virtual_address_bits,
		.physical_address_bits = g_paging_hw_physical_address_bits,
	};

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();

	// Create two address spaces with different ASIDs.
	static constexpr std::uint16_t kAsidA = 1;
	static constexpr std::uint16_t kAsidB = 2;

	const auto address_space_a_or = AddressSpace::Create(&pmm, address_bits, kAsidA);
	ROCINANTE_EXPECT_TRUE(ctx, address_space_a_or.has_value());
	if (!address_space_a_or.has_value()) return;
	const auto address_space_b_or = AddressSpace::Create(&pmm, address_bits, kAsidB);
	ROCINANTE_EXPECT_TRUE(ctx, address_space_b_or.has_value());
	if (!address_space_b_or.has_value()) return;

	const AddressSpace address_space_a = address_space_a_or.value();
	const AddressSpace address_space_b = address_space_b_or.value();

	const PagePermissions kernel_identity_permissions{
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

	const PagePermissions physmap_permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	// Map the kernel image identity range into both roots.
	const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
	const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);
	ROCINANTE_EXPECT_TRUE(ctx, kernel_physical_end > kernel_physical_base);

	const std::uintptr_t kernel_size_bytes = kernel_physical_end - kernel_physical_base;
	const std::size_t kernel_size_rounded =
		static_cast<std::size_t>((kernel_size_bytes + Rocinante::Memory::Paging::kPageSizeBytes - 1) &
			~(Rocinante::Memory::Paging::kPageSizeBytes - 1));

	ROCINANTE_EXPECT_TRUE(ctx, address_space_a.MapRange4KiB(
		&pmm,
		kernel_physical_base,
		kernel_physical_base,
		kernel_size_rounded,
		kernel_identity_permissions));
	ROCINANTE_EXPECT_TRUE(ctx, address_space_b.MapRange4KiB(
		&pmm,
		kernel_physical_base,
		kernel_physical_base,
		kernel_size_rounded,
		kernel_identity_permissions));

	// Map MMIO pages needed for test logging/shutdown.
	static constexpr std::uintptr_t kUartPhysicalBase = 0x1fe001e0ull;
	static constexpr std::uintptr_t kSysconPhysicalBase = 0x100e001cull;
	const std::uintptr_t uart_page_base = kUartPhysicalBase & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	const std::uintptr_t syscon_page_base = kSysconPhysicalBase & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	ROCINANTE_EXPECT_TRUE(ctx, address_space_a.MapRange4KiB(
		&pmm, uart_page_base, uart_page_base, Rocinante::Memory::Paging::kPageSizeBytes, mmio_permissions));
	ROCINANTE_EXPECT_TRUE(ctx, address_space_b.MapRange4KiB(
		&pmm, uart_page_base, uart_page_base, Rocinante::Memory::Paging::kPageSizeBytes, mmio_permissions));
	ROCINANTE_EXPECT_TRUE(ctx, address_space_a.MapRange4KiB(
		&pmm, syscon_page_base, syscon_page_base, Rocinante::Memory::Paging::kPageSizeBytes, mmio_permissions));
	ROCINANTE_EXPECT_TRUE(ctx, address_space_b.MapRange4KiB(
		&pmm, syscon_page_base, syscon_page_base, Rocinante::Memory::Paging::kPageSizeBytes, mmio_permissions));

	// Map a physmap window that covers the PMM-tracked range for page-table access in mapped mode.
	// This must exist in BOTH roots, because switching PGDL changes the mappings used for the physmap.
	static constexpr std::uintptr_t kTrackedPhysicalBase = 0x01000000ull; // 16 MiB (matches earlier paging smoke test)
	static constexpr std::size_t kTrackedSizeBytes = 32u * 1024u * 1024u; // 32 MiB
	const std::uintptr_t physmap_virtual_base =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(kTrackedPhysicalBase, address_bits.virtual_address_bits);

	ROCINANTE_EXPECT_TRUE(ctx, address_space_a.MapRange4KiB(
		&pmm,
		physmap_virtual_base,
		kTrackedPhysicalBase,
		kTrackedSizeBytes,
		physmap_permissions));
	ROCINANTE_EXPECT_TRUE(ctx, address_space_b.MapRange4KiB(
		&pmm,
		physmap_virtual_base,
		kTrackedPhysicalBase,
		kTrackedSizeBytes,
		physmap_permissions));

	// Allocate two different physical pages and fill them with different sentinel values.
	const auto page_a_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, page_a_or.has_value());
	if (!page_a_or.has_value()) return;
	const auto page_b_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, page_b_or.has_value());
	if (!page_b_or.has_value()) return;

	static constexpr std::uint64_t kSentinelA = 0xaaaaaaaaaaaaaaaaull;
	static constexpr std::uint64_t kSentinelB = 0xbbbbbbbbbbbbbbbbull;
	const std::uintptr_t page_a_phys = page_a_or.value();
	const std::uintptr_t page_b_phys = page_b_or.value();
	const std::uintptr_t page_a_virt =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(page_a_phys, address_bits.virtual_address_bits);
	const std::uintptr_t page_b_virt =
		Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(page_b_phys, address_bits.virtual_address_bits);
	*reinterpret_cast<volatile std::uint64_t*>(page_a_virt) = kSentinelA;
	*reinterpret_cast<volatile std::uint64_t*>(page_b_virt) = kSentinelB;

	// Map the same low-half VA to different physical pages.
	static constexpr std::uintptr_t kTestVirtualPageBase = kPagingHwScratchVirtualPageBase;
	static_assert((kTestVirtualPageBase % Rocinante::Memory::Paging::kPageSizeBytes) == 0);

	const PagePermissions user_page_permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = false,
	};

	ROCINANTE_EXPECT_TRUE(ctx, address_space_a.MapPage4KiB(&pmm, kTestVirtualPageBase, page_a_phys, user_page_permissions));
	ROCINANTE_EXPECT_TRUE(ctx, address_space_b.MapPage4KiB(&pmm, kTestVirtualPageBase, page_b_phys, user_page_permissions));

	// Save current context so we can restore after the test.
	// CSR numbering matches the LoongArch privileged architecture spec.
	// - CSR.PGDL is 0x19
	// - CSR.ASID is 0x18
	static constexpr std::uint32_t kCsrAsid = 0x18;
	static constexpr std::uint32_t kCsrPgdl = 0x19;
	std::uint64_t old_asid;
	std::uint64_t old_pgdl;
	asm volatile("csrrd %0, %1" : "=r"(old_asid) : "i"(kCsrAsid));
	asm volatile("csrrd %0, %1" : "=r"(old_pgdl) : "i"(kCsrPgdl));

	// Switch to A and observe sentinel A.
	Rocinante::Memory::PagingHw::ActivateLowHalfAddressSpace(address_space_a.LowHalfRoot(), address_space_a.AddressSpaceId());
	const std::uint64_t observed_a = *reinterpret_cast<volatile std::uint64_t*>(kTestVirtualPageBase);
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_a, kSentinelA);

	// Switch to B and observe sentinel B.
	Rocinante::Memory::PagingHw::ActivateLowHalfAddressSpace(address_space_b.LowHalfRoot(), address_space_b.AddressSpaceId());
	const std::uint64_t observed_b = *reinterpret_cast<volatile std::uint64_t*>(kTestVirtualPageBase);
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_b, kSentinelB);

	// Restore the previous address space/root.
	asm volatile("csrwr %0, %1" :: "r"(old_asid), "i"(kCsrAsid));
	asm volatile("csrwr %0, %1" :: "r"(old_pgdl), "i"(kCsrPgdl));
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();
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

void TestEntry_PagingHw_EnablePaging_TlbRefillSmoke(TestContext* ctx) {
	Test_PagingHw_EnablePaging_TlbRefillSmoke(ctx);
}

void TestEntry_PagingHw_PostPaging_MapUnmap_Faults(TestContext* ctx) {
	Test_PagingHw_PostPaging_MapUnmap_Faults(ctx);
}

void TestEntry_PagingHw_HigherHalfStack_GuardPageFaults(TestContext* ctx) {
	Test_PagingHw_HigherHalfStack_GuardPageFaults(ctx);
}

void TestEntry_PagingHw_AddressSpaces_SwitchPgdlChangesTranslation(TestContext* ctx) {
	Test_PagingHw_AddressSpaces_SwitchPgdlChangesTranslation(ctx);
}

void TestEntry_PagingHw_UnmappedAccess_FaultsAndReportsBadV(TestContext* ctx) {
	Test_PagingHw_UnmappedAccess_FaultsAndReportsBadV(ctx);
}

void TestEntry_PagingHw_PagingFaultObserver_DispatchesAndCanHandle(TestContext* ctx) {
	Test_PagingHw_PagingFaultObserver_DispatchesAndCanHandle(ctx);
}

void TestEntry_PagingHw_PagingFaultObserver_MapsAndRetries(TestContext* ctx) {
	Test_PagingHw_PagingFaultObserver_MapsAndRetries(ctx);
}

void TestEntry_PagingHw_ReadOnlyStore_RaisesPme(TestContext* ctx) {
	Test_PagingHw_ReadOnlyStore_RaisesPme(ctx);
}

void TestEntry_PagingHw_NonExecutableFetch_RaisesPnx(TestContext* ctx) {
	Test_PagingHw_NonExecutableFetch_RaisesPnx(ctx);
}

} // namespace Rocinante::Testing

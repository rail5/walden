/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/kernel/paging_bringup.h>

#include <src/memory/memory.h>
#include <src/memory/heap.h>
#include <src/memory/kernel_va_allocator.h>
#include <src/memory/paging.h>
#include <src/memory/paging_hw.h>
#include <src/memory/paging_state.h>
#include <src/memory/pmm.h>
#include <src/memory/virtual_layout.h>
#include <src/platform/console.h>
#include <src/platform/power.h>
#include <src/platform/qemu_virt.h>
#include <src/sp/cpucfg.h>
#include <src/sp/uart16550.h>
#include <src/helpers/optional.h>
#include <src/trap/trap.h>

#include <cstddef>
#include <cstdint>

namespace {

extern "C" char _start;
extern "C" char _end;
extern "C" void __exception_entry();

// Paging bring-up handoff state.
//
// These values are populated while building the bootstrap page tables (paging
// still off), then consumed after paging is enabled and we have switched to a
// higher-half stack.
static std::uintptr_t g_paging_bringup_heap_virtual_base = 0;
static std::size_t g_paging_bringup_heap_size_bytes = 0;

// Post-paging continuation.
//
// This is a function pointer to code within the kernel image. It is recorded
// before paging is enabled (while code is executing in low/direct addressing),
// then invoked after paging is enabled by jumping to the higher-half alias of
// the same function.
static std::uintptr_t g_post_paging_continuation_low = 0;

[[noreturn]] [[gnu::noinline]] static void PagingBringup_HigherHalfStackContinuation() {
	auto& uart = Rocinante::Platform::GetEarlyUart();

	// This function is entered via an assembly jump after paging is enabled.
	// It is the first C++ code we intentionally run with a higher-half stack.

	std::uintptr_t current_pc = 0;
	asm volatile(
		"la.local %0, 1f\n"
		"1:\n"
		: "=r"(current_pc)
	);

	std::uintptr_t current_sp = 0;
	asm volatile("move %0, $sp" : "=r"(current_sp));

	uart.puts("Paging bring-up: higher-half stack continuation entered; pc=");
	uart.write_dec_u64(current_pc);
	uart.putc('\n');
	uart.puts("Paging bring-up: higher-half stack continuation sp=");
	uart.write_dec_u64(current_sp);
	uart.putc('\n');

	// Higher-half exception entry relocation.
	//
	// Goal:
	// Prove that general exceptions can enter/return using the higher-half kernel
	// alias, so later we can safely tear down the low identity mapping.
	//
	// Spec anchors (LoongArch-Vol1-EN.html):
	// - Section 6.3.1 (Exception Entry): general exceptions use CSR.EENTRY.
	// - Section 6.3.4 (Hardware Exception Handling of TLB Refill Exception): TLB
	//   refill uses CSR.TLBRENTRY and forces direct address translation mode.
	//
	// Policy:
	// - Relocate CSR.EENTRY/CSR.MERRENTRY to the higher-half alias.
	// - Do not change CSR.TLBRENTRY here.
	const Rocinante::Memory::PagingState* paging_state = Rocinante::Memory::TryGetPagingState();
	if (paging_state && paging_state->address_bits.virtual_address_bits != 0) {
		const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
		const std::uintptr_t kernel_higher_half_base =
			Rocinante::Memory::VirtualLayout::KernelHigherHalfBase(paging_state->address_bits.virtual_address_bits);
		const std::uintptr_t exception_entry_low = reinterpret_cast<std::uintptr_t>(&__exception_entry);
		const std::uintptr_t exception_entry_high = kernel_higher_half_base + (exception_entry_low - kernel_physical_base);

		uart.puts("Paging bring-up: relocating EENTRY/MERRENTRY to entry=");
		uart.write_dec_u64(exception_entry_high);
		uart.putc('\n');

		Rocinante::Trap::SetGeneralAndMachineErrorExceptionEntryPageBase(exception_entry_high);

		uart.puts("Paging bring-up: triggering BREAK self-check\n");
		asm volatile("break 0" ::: "memory");
		uart.puts("Paging bring-up: BREAK returned\n");
	} else {
		uart.puts("Paging bring-up: skipping EENTRY relocation (no address_bits)\n");
	}

	// Heap handoff: re-initialize the allocator to use the VM-backed heap region
	// we mapped during paging bring-up.
	if (g_paging_bringup_heap_virtual_base != 0 && g_paging_bringup_heap_size_bytes != 0) {
		uart.puts("Paging bring-up: initializing heap after paging; heap_base=");
		uart.write_dec_u64(g_paging_bringup_heap_virtual_base);
		uart.puts(" heap_size_bytes=");
		uart.write_dec_u64(g_paging_bringup_heap_size_bytes);
		uart.putc('\n');

		Rocinante::Memory::InitHeapAfterPaging(
			reinterpret_cast<void*>(g_paging_bringup_heap_virtual_base),
			g_paging_bringup_heap_size_bytes
		);

		uart.puts("Paging bring-up: heap stats after init: total_bytes=");
		uart.write_dec_u64(Rocinante::Memory::Heap::TotalBytes());
		uart.puts(" free_bytes=");
		uart.write_dec_u64(Rocinante::Memory::Heap::FreeBytes());
		uart.putc('\n');

		void* p = Rocinante::Memory::Heap::Alloc(64, 16);
		uart.puts("Paging bring-up: heap alloc(64,16) returned ");
		uart.write_hex_u64(reinterpret_cast<std::uint64_t>(p));
		uart.putc('\n');
		if (p) {
			Rocinante::Memory::Heap::Free(p);
		}
	} else {
		uart.puts("Paging bring-up: heap after paging not configured; skipping heap handoff\n");
	}

	// Post-paging software-walker self-check.
	//
	// Purpose:
	// Prove that our software page-table walker can read page-table pages while
	// paging is enabled.
	//
	// Why this matters:
	// In mapped address translation mode, the paging module dereferences page-table
	// pages through the physmap (not by treating physical addresses as pointers).
	// If the physmap does not cover the page-table pages, walking will fault.
	//
	// Spec anchor:
	// - LoongArch-Vol1-EN.html, Section 5.2: CRMD.DA/CRMD.PG select direct vs mapped
	//   address translation mode.
	if (paging_state && paging_state->root.root_physical_address != 0 && paging_state->address_bits.virtual_address_bits != 0) {
		const Rocinante::Memory::Paging::PageTableRoot root{
			.root_physical_address = paging_state->root.root_physical_address,
		};

		const std::uintptr_t kernel_higher_half_base =
			Rocinante::Memory::VirtualLayout::KernelHigherHalfBase(paging_state->address_bits.virtual_address_bits);
		const std::uintptr_t physmap_root_virtual =
			Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(
				paging_state->root.root_physical_address,
				paging_state->address_bits.virtual_address_bits
			);

		uart.puts("Paging bring-up: post-paging Translate self-check\n");
		uart.puts("Paging bring-up:   hh_base=");
		uart.write_dec_u64(kernel_higher_half_base);
		uart.putc('\n');
		uart.puts("Paging bring-up:   physmap(root_pt)=");
		uart.write_dec_u64(physmap_root_virtual);
		uart.putc('\n');

		const auto translated_hh = Rocinante::Memory::Paging::Translate(root, kernel_higher_half_base, paging_state->address_bits);
		uart.puts("Paging bring-up:   Translate(hh_base)=");
		if (translated_hh.has_value()) {
			uart.write_dec_u64(translated_hh.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');

		const auto translated_physmap = Rocinante::Memory::Paging::Translate(root, physmap_root_virtual, paging_state->address_bits);
		uart.puts("Paging bring-up:   Translate(physmap(root_pt))=");
		if (translated_physmap.has_value()) {
			uart.write_dec_u64(translated_physmap.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');

		// ---------------------------------------------------------------------
		// Tear down the low kernel identity mapping.
		//
		// After switching execution to the higher-half alias, the kernel should no
		// longer require the low identity mapping of its own image.
		//
		// Spec anchors (LoongArch-Vol1-EN.html):
		// - Section 4.2.4.7 (INVTLB): INVTLB maintains consistency between the TLB
		//   and page table data in memory. After changing page tables, we must
		//   invalidate stale TLB entries.
		// - Section 5.2 (Address Translation Mode): mapped mode uses the page-table
		//   mappings; if we remove a mapping, accesses through that VA should fault.
		//
		// Policy:
		// - Unmap the kernel's low identity range from the bootstrap page tables.
		// - Flush TLB entries (INVTLB op=0).
		// - Log software translations before/after as a sanity check.
		{
			const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
			const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);
			const std::uintptr_t kernel_size_bytes = (kernel_physical_end > kernel_physical_base)
				? (kernel_physical_end - kernel_physical_base)
				: 0;
			const std::size_t map_size_rounded =
				static_cast<std::size_t>((kernel_size_bytes + Rocinante::Memory::Paging::kPageSizeBytes - 1) &
					~(Rocinante::Memory::Paging::kPageSizeBytes - 1));

			uart.puts("Paging bring-up: tearing down low kernel identity mapping\n");
			uart.puts("Paging bring-up:   low kernel base=");
			uart.write_dec_u64(kernel_physical_base);
			uart.puts(" size_bytes=");
			uart.write_dec_u64(map_size_rounded);
			uart.putc('\n');

			const auto translated_low_before =
				Rocinante::Memory::Paging::Translate(root, kernel_physical_base, paging_state->address_bits);
			uart.puts("Paging bring-up:   Translate(low_kernel_base) before=");
			if (translated_low_before.has_value()) {
				uart.write_dec_u64(translated_low_before.value());
			} else {
				uart.puts("<none>");
			}
			uart.putc('\n');

			if (map_size_rounded == 0) {
				uart.puts("Paging bring-up:   identity teardown skipped (zero kernel size)\n");
			} else {
				for (std::size_t offset_bytes = 0; offset_bytes < map_size_rounded; offset_bytes += Rocinante::Memory::Paging::kPageSizeBytes) {
					const std::uintptr_t low_virtual_page_base =
						kernel_physical_base + static_cast<std::uintptr_t>(offset_bytes);
					(void)Rocinante::Memory::Paging::UnmapPage4KiB(root, low_virtual_page_base, paging_state->address_bits);
				}

				uart.puts("Paging bring-up:   INVTLB op=0 (flush all)\n");
				Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();
			}

			const auto translated_low_after =
				Rocinante::Memory::Paging::Translate(root, kernel_physical_base, paging_state->address_bits);
			uart.puts("Paging bring-up:   Translate(low_kernel_base) after=");
			if (translated_low_after.has_value()) {
				uart.write_dec_u64(translated_low_after.value());
			} else {
				uart.puts("<none>");
			}
			uart.putc('\n');

			uart.puts("Paging bring-up: low kernel identity mapping torn down\n");
		}
	} else {
		uart.puts("Paging bring-up: post-paging Translate self-check skipped (no root/address_bits)\n");
	}

	// Transfer control to the caller-supplied continuation.
	//
	// Goal:
	// This is the bridge from "paging bring-up" to "normal kernel execution".
	// We keep all existing bring-up self-checks above, then hand off to the
	// kernel proper in mapped mode.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 7.5.6 (PGDH): the higher half is selected when VA[VALEN-1]==1.
	//   We therefore jump to a higher-half alias of the continuation.
	if (g_post_paging_continuation_low != 0 && paging_state && paging_state->address_bits.virtual_address_bits != 0) {
		const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
		const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);
		const std::uintptr_t continuation_low = g_post_paging_continuation_low;

		if (continuation_low < kernel_physical_base || continuation_low >= kernel_physical_end) {
			uart.puts("Paging bring-up: post-paging continuation is outside kernel image; refusing to jump\n");
			uart.puts("Paging bring-up:   kernel_phys=[");
			uart.write_dec_u64(kernel_physical_base);
			uart.puts(", ");
			uart.write_dec_u64(kernel_physical_end);
			uart.puts(") continuation_low=");
			uart.write_dec_u64(continuation_low);
			uart.putc('\n');
			Rocinante::Platform::Halt();
		}

		const std::uintptr_t kernel_higher_half_base =
			Rocinante::Memory::VirtualLayout::KernelHigherHalfBase(paging_state->address_bits.virtual_address_bits);
		const std::uintptr_t continuation_offset = continuation_low - kernel_physical_base;
		const std::uintptr_t continuation_high = kernel_higher_half_base + continuation_offset;

		uart.puts("Paging bring-up: transferring control to mapped continuation target=");
		uart.write_dec_u64(continuation_high);
		uart.putc('\n');

		asm volatile(
			"jirl $zero, %0, 0\n"
			"break 0\n"
			:
			: "r"(continuation_high)
			: "memory"
		);
		__builtin_unreachable();
	}

	auto& cpucfg = Rocinante::GetCPUCFG();

	uart.puts("Hello, Rocinante!\n");
	uart.puts("Don the LoongArch64 armor and prepare to ride!\n\n");

	if (cpucfg.MMUSupportsPageMappingMode()) {
		uart.puts("MMU supports page mapping mode\n");
	} else {
		uart.puts("MMU does not support page mapping mode\n");
	}

	uart.putc('\n');

	uart.puts("Supported virtual address bits (VALEN): ");
	uart.write_dec_u64(cpucfg.VirtualAddressBits());
	uart.putc('\n');
	uart.puts("Supported physical address bits (PALEN): ");
	uart.write_dec_u64(cpucfg.PhysicalAddressBits());
	uart.putc('\n');

	uart.putc('\n');

	Rocinante::Platform::Halt();
}

} // namespace

namespace Rocinante::Kernel {

void RunPagingBringup(
	Rocinante::Uart16550& uart,
	Rocinante::Memory::PhysicalMemoryManager& pmm,
	std::uintptr_t kernel_physical_base,
	std::uintptr_t kernel_physical_end,
	void (*post_paging_continuation)()
) {
	uart.puts("\nPaging bring-up: building bootstrap page tables\n");

	// The continuation must live within the kernel image so we can jump to a
	// higher-half alias of it after paging is enabled.
	g_post_paging_continuation_low = reinterpret_cast<std::uintptr_t>(post_paging_continuation);

	const std::uint32_t virtual_address_bits = Rocinante::GetCPUCFG().VirtualAddressBits();
	const std::uint32_t physical_address_bits = Rocinante::GetCPUCFG().PhysicalAddressBits();
	uart.puts("Paging bring-up: CPUCFG VALEN=");
	uart.write_dec_u64(virtual_address_bits);
	uart.puts(" PALEN=");
	uart.write_dec_u64(physical_address_bits);
	uart.putc('\n');

	constexpr auto BitIndexFromSingleBitMask = [](std::uint64_t mask) constexpr -> std::uint8_t {
		std::uint8_t index = 0;
		while (((mask >> index) & 0x1ull) == 0) {
			index++;
		}
		return index;
	};
	constexpr std::uint8_t kLowestHighFlagBit =
		(BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoRead) <
			BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoExecute))
			? BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoRead)
			: BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoExecute);
	constexpr std::uint32_t kMaxEncodablePALEN = kLowestHighFlagBit;
	if (physical_address_bits < Rocinante::Memory::Paging::kPageShiftBits || physical_address_bits > kMaxEncodablePALEN) {
		uart.puts("Paging bring-up: unsupported PALEN for current PTE encoding; skipping.\n");
		return;
	}

	const Rocinante::Memory::Paging::AddressSpaceBits address_bits{
		.virtual_address_bits = static_cast<std::uint8_t>(virtual_address_bits),
		.physical_address_bits = static_cast<std::uint8_t>(physical_address_bits),
	};

	// Bootstrap physmap policy (bring-up): map a linear window of physical RAM
	// into the higher half.
	//
	// Correctness invariant:
	// Once we enable paging (CRMD.DA=0, CRMD.PG=1), page-table pages can no
	// longer be safely accessed by treating their physical address as a
	// pointer. All physical memory the kernel intends to touch must be mapped.
	//
	// Spec anchor:
	// - LoongArch-Vol1-EN.html, Section 5.2 (Virtual Address Space and Address
	//   Translation Mode): CRMD.DA/CRMD.PG select direct vs mapped translation.
	//
	const std::uintptr_t physmap_physical_base = pmm.TrackedPhysicalBase();
	const std::uintptr_t tracked_physical_limit = pmm.TrackedPhysicalLimit();
	std::size_t physmap_size_bytes = 0;
	if (tracked_physical_limit > physmap_physical_base) {
		const std::size_t tracked_size_bytes =
			static_cast<std::size_t>(tracked_physical_limit - physmap_physical_base);
		// Bring-up policy:
		// Expand the physmap window to cover the entire PMM-tracked RAM span so
		// that any PMM allocation can be accessed via the linear physmap once
		// paging is enabled.
		physmap_size_bytes = tracked_size_bytes;
		physmap_size_bytes &= ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	}

	if (physmap_size_bytes == 0) {
		uart.puts("Paging bring-up: no tracked RAM for physmap; skipping physmap build\n");
	}

	const auto root_or = Rocinante::Memory::Paging::AllocateRootPageTable(&pmm);
	if (!root_or.has_value()) {
		uart.puts("Paging bring-up: failed to allocate root page table\n");
		return;
	}
	const auto root = root_or.value();
	Rocinante::Memory::InitializePagingState(Rocinante::Memory::PagingState{
		.root = root,
		.address_bits = address_bits,
	});

	// Identity-map the kernel image so enabling paging does not immediately
	// fault while executing in the low physical mapping.
	const std::uintptr_t kernel_size_bytes = kernel_physical_end - kernel_physical_base;
	const std::size_t map_size_rounded =
		static_cast<std::size_t>((kernel_size_bytes + Rocinante::Memory::Paging::kPageSizeBytes - 1) &
				~(Rocinante::Memory::Paging::kPageSizeBytes - 1));

	Rocinante::Memory::Paging::PagePermissions kernel_permissions{
		.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
		.execute = Rocinante::Memory::Paging::ExecutePermissions::Executable,
		.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
		.global = true,
	};

	if (!Rocinante::Memory::Paging::MapRange4KiB(
		&pmm,
		root,
		kernel_physical_base,
		kernel_physical_base,
		map_size_rounded,
		kernel_permissions,
		address_bits
	)) {
		uart.puts("Paging bring-up: failed to map kernel identity range\n");
		return;
	}

	// Map a higher-half alias of the kernel image.
	//
	// Spec anchor:
	// - LoongArch Reference Manual Vol 1 (v1.10)
	//   - Section 7.5.6 (PGDH): higher half is selected when VA[VALEN-1]==1.
	//   - Section 5.2: mapped address translation mode legality depends on
	//     the implemented virtual address width (VALEN and optional RVACFG.RDVA).
	const std::uintptr_t kernel_higher_half_base =
		Rocinante::Memory::VirtualLayout::KernelHigherHalfBase(address_bits.virtual_address_bits);
	std::uintptr_t higher_half_stack_top = 0;
	Rocinante::Memory::KernelVirtualAddressAllocator kernel_va;

	if (!Rocinante::Memory::Paging::MapRange4KiB(
		&pmm,
		root,
		kernel_higher_half_base,
		kernel_physical_base,
		map_size_rounded,
		kernel_permissions,
		address_bits
	)) {
		uart.puts("Paging bring-up: failed to map kernel higher-half alias\n");
	} else {
		uart.puts("Paging bring-up: kernel higher-half base=");
		uart.write_dec_u64(kernel_higher_half_base);
		uart.putc('\n');

		// Bring-up policy: allocate additional kernel virtual ranges out of the
		// gap between the kernel higher-half mapping and the physmap region.
		const std::uintptr_t kernel_higher_half_end =
			(kernel_higher_half_base + map_size_rounded + (Rocinante::Memory::Paging::kPageSizeBytes - 1)) &
			~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
		const std::uintptr_t physmap_base =
			Rocinante::Memory::VirtualLayout::PhysMapBase(address_bits.virtual_address_bits);
		if (kernel_higher_half_end >= physmap_base) {
			uart.puts("Paging bring-up: no VA space between higher-half kernel and physmap; skipping stack/heap mapping\n");
		} else {
			kernel_va.Init(kernel_higher_half_end, physmap_base);
		}
	}

	// Identity-map the UART and syscon MMIO pages so existing raw MMIO pointers
	// remain usable immediately after paging is enabled.
	//
	// Correctness pitfall (per plan / LoongArch spec): caching attributes.
	// These must be mapped as an uncached/strongly-ordered memory type, or
	// early UART/debug output can become flaky.
	Rocinante::Memory::Paging::PagePermissions mmio_permissions{
		.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
		.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
		.cache = Rocinante::Memory::Paging::CacheMode::StrongUncached,
		.global = true,
	};

	const std::uintptr_t UART_BASE = Rocinante::Platform::QemuVirt::kUartBase;
	const std::uintptr_t kSysconBase = Rocinante::Platform::QemuVirt::kSysconBase;

	const std::uintptr_t uart_page_base =
		UART_BASE & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	if (!Rocinante::Memory::Paging::MapRange4KiB(
		&pmm,
		root,
		uart_page_base,
		uart_page_base,
		Rocinante::Memory::Paging::kPageSizeBytes,
		mmio_permissions,
		address_bits
	)) {
		uart.puts("Paging bring-up: failed to map UART MMIO page\n");
	}

	const std::uintptr_t syscon_page_base =
		kSysconBase & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
	if (!Rocinante::Memory::Paging::MapRange4KiB(
		&pmm,
		root,
		syscon_page_base,
		syscon_page_base,
		Rocinante::Memory::Paging::kPageSizeBytes,
		mmio_permissions,
		address_bits
	)) {
		uart.puts("Paging bring-up: failed to map syscon-poweroff MMIO page\n");
	}

	// Allocate and map a higher-half kernel stack region.
	//
	// Spec-driven constraint:
	// For VALEN=N, the lowest canonical higher-half address is
	// KernelHigherHalfBase. Any address below it is non-canonical and will
	// fail our VA canonicalization checks.
	//
	// Bring-up policy: place the stack just above the kernel higher-half
	// alias range.
	//
	// Guard-page policy:
	// The stack grows downward, so leave one unmapped guard page below the
	// mapped stack region.
	{
		static constexpr std::size_t kHigherHalfStackGuardPageCount = 1;
		static constexpr std::size_t kHigherHalfStackMappedPageCount = 4;

		const auto stack_region_or = kernel_va.Allocate(
			(kHigherHalfStackGuardPageCount + kHigherHalfStackMappedPageCount) * Rocinante::Memory::Paging::kPageSizeBytes,
			Rocinante::Memory::Paging::kPageSizeBytes
		);
		if (!stack_region_or.has_value()) {
			uart.puts("Paging bring-up: failed to allocate higher-half stack VA range\n");
		} else {
			const std::uintptr_t stack_guard_virtual_base = stack_region_or.value();
			const std::uintptr_t stack_virtual_base =
				stack_guard_virtual_base + (kHigherHalfStackGuardPageCount * Rocinante::Memory::Paging::kPageSizeBytes);
			const std::uintptr_t stack_virtual_top =
				stack_virtual_base + (kHigherHalfStackMappedPageCount * Rocinante::Memory::Paging::kPageSizeBytes);

			Rocinante::Memory::Paging::PagePermissions stack_permissions{
				.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
				.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
				.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
				.global = true,
			};

			std::uintptr_t stack_physical_pages[kHigherHalfStackMappedPageCount] = {};
			bool stack_ok = true;

			for (std::size_t i = 0; i < kHigherHalfStackMappedPageCount; i++) {
				const auto stack_page_or = pmm.AllocatePage();
				if (!stack_page_or.has_value()) {
					uart.puts("Paging bring-up: failed to allocate higher-half stack page\n");
					stack_ok = false;
					break;
				}

				stack_physical_pages[i] = stack_page_or.value();
				const std::uintptr_t page_virtual = stack_virtual_base + (i * Rocinante::Memory::Paging::kPageSizeBytes);
				if (!Rocinante::Memory::Paging::MapRange4KiB(
					&pmm,
					root,
					page_virtual,
					stack_physical_pages[i],
					Rocinante::Memory::Paging::kPageSizeBytes,
					stack_permissions,
					address_bits
				)) {
					uart.puts("Paging bring-up: failed to map higher-half stack page\n");
					uart.puts("Paging bring-up: NOTE: stack mapping failure may leave partial mappings\n");
					stack_ok = false;
					break;
				}
			}

			if (stack_ok) {
				higher_half_stack_top = stack_virtual_top;
				uart.puts("Paging bring-up: higher-half stack mapped; guard_virt_base=");
				uart.write_dec_u64(stack_guard_virtual_base);
				uart.puts(" stack_virt_base=");
				uart.write_dec_u64(stack_virtual_base);
				uart.puts(" stack_virt_top=");
				uart.write_dec_u64(stack_virtual_top);
				uart.puts(" pages=");
				uart.write_dec_u64(kHigherHalfStackMappedPageCount);
				uart.puts(" phys_pages=[");
				for (std::size_t i = 0; i < kHigherHalfStackMappedPageCount; i++) {
					if (i != 0) uart.puts(", ");
					uart.write_dec_u64(stack_physical_pages[i]);
				}
				uart.puts("]\n");
			}
		}
	}

	// Allocate and map a small VM-backed heap region.
	//
	// Plan alignment:
	// - This is the handoff from the bootstrap .bss heap to a region backed
	//   by real PMM frames and page-table mappings.
	// - We keep the bootstrap heap alive; this is bring-up, not a teardown.
	//
	// Placement policy (bring-up only):
	// Place heap pages immediately above the higher-half stack region.
	// This avoids overlapping the stack guard+stack pages we just mapped.
	{
		static constexpr std::size_t kHeapPageCount = 16;
		static constexpr std::size_t kHeapSizeBytes = kHeapPageCount * Rocinante::Memory::Paging::kPageSizeBytes;

		if (higher_half_stack_top == 0) {
			uart.puts("Paging bring-up: higher-half stack not mapped; skipping heap mapping\n");
		} else {
			const auto heap_virtual_or =
				kernel_va.Allocate(kHeapSizeBytes, Rocinante::Memory::Paging::kPageSizeBytes);
			if (!heap_virtual_or.has_value()) {
				uart.puts("Paging bring-up: failed to allocate heap VA range\n");
			} else {
				const std::uintptr_t heap_virtual_base = heap_virtual_or.value();

			Rocinante::Memory::Paging::PagePermissions heap_permissions{
				.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
				.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
				.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
				.global = true,
			};

			bool heap_ok = true;
			for (std::size_t i = 0; i < kHeapPageCount; i++) {
				const auto page_or = pmm.AllocatePage();
				if (!page_or.has_value()) {
					uart.puts("Paging bring-up: failed to allocate heap page\n");
					heap_ok = false;
					break;
				}
				const std::uintptr_t heap_page_physical = page_or.value();
				const std::uintptr_t heap_page_virtual = heap_virtual_base + (i * Rocinante::Memory::Paging::kPageSizeBytes);
				if (!Rocinante::Memory::Paging::MapPage4KiB(
					&pmm,
					root,
					heap_page_virtual,
					heap_page_physical,
					heap_permissions,
					address_bits
				)) {
					uart.puts("Paging bring-up: failed to map heap page\n");
					uart.puts("Paging bring-up: NOTE: heap mapping failure may leave partial mappings\n");
					heap_ok = false;
					break;
				}
			}

				if (heap_ok) {
					g_paging_bringup_heap_virtual_base = heap_virtual_base;
					g_paging_bringup_heap_size_bytes = kHeapSizeBytes;
					uart.puts("Paging bring-up: higher-half heap mapped; virt_base=");
					uart.write_dec_u64(heap_virtual_base);
					uart.puts(" size_bytes=");
					uart.write_dec_u64(kHeapSizeBytes);
					uart.puts(" pages=");
					uart.write_dec_u64(kHeapPageCount);
					uart.putc('\n');
				}
			}
		}
	}

	// Bootstrap physmap: map the previously computed physmap window into the
	// higher half.
	if (physmap_size_bytes != 0) {
		const std::uintptr_t physmap_virtual_base =
			Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(
				physmap_physical_base,
				address_bits.virtual_address_bits
			);

		Rocinante::Memory::Paging::PagePermissions physmap_permissions{
			.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
			.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
			.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
			.global = true,
		};

		if (!Rocinante::Memory::Paging::MapRange4KiB(
			&pmm,
			root,
			physmap_virtual_base,
			physmap_physical_base,
			physmap_size_bytes,
			physmap_permissions,
			address_bits
		)) {
			uart.puts("Paging bring-up: failed to map bootstrap physmap window\n");
		} else {
			const std::uintptr_t physmap_physical_limit = physmap_physical_base + physmap_size_bytes;
			uart.puts("Paging bring-up: physmap virt_base=");
			uart.write_dec_u64(physmap_virtual_base);
			uart.puts(" phys=[");
			uart.write_dec_u64(physmap_physical_base);
			uart.puts(", ");
			uart.write_dec_u64(physmap_physical_limit);
			uart.puts(")\n");
		}
	}

	uart.puts("Paging bring-up: root_pt_phys=");
	uart.write_dec_u64(root.root_physical_address);
	uart.puts(" kernel_phys=[");
	uart.write_dec_u64(kernel_physical_base);
	uart.puts(", ");
	uart.write_dec_u64(kernel_physical_end);
	uart.puts(")\n");

	// Bring-up self-check: confirm the software-built tables contain a
	// translation for a kernel address before enabling paging.
	//
	// This helps distinguish "page tables not populated" from
	// "TLB refill walk mismatch".
	const auto DumpPagingProbe = [&](std::uintptr_t probe_va) {
		uart.puts("Paging bring-up: probe_va=");
		uart.write_dec_u64(probe_va);
		uart.putc('\n');

		const auto translated = Rocinante::Memory::Paging::Translate(root, probe_va, address_bits);
		uart.puts("Paging bring-up: translate=");
		if (translated.has_value()) {
			uart.write_dec_u64(translated.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');

		// Assumes 4-level, 4 KiB paging layout for the current QEMU bring-up
		// configuration: Dir3 -> Dir2 -> Dirl -> PT -> 4 KiB page.
		// (Indices are 9 bits each.)
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

		uart.puts("Paging bring-up: idx d3=");
		uart.write_dec_u64(idx_dir3);
		uart.puts(" d2=");
		uart.write_dec_u64(idx_dir2);
		uart.puts(" dl=");
		uart.write_dec_u64(idx_dirl);
		uart.puts(" pt=");
		uart.write_dec_u64(idx_pt);
		uart.putc('\n');

		auto* dir3 = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(root.root_physical_address);
		const auto IsWalkable = [](std::uint64_t entry) {
			return (entry & (Rocinante::Memory::Paging::PteBits::kValid | Rocinante::Memory::Paging::PteBits::kPresent)) ==
				(Rocinante::Memory::Paging::PteBits::kValid | Rocinante::Memory::Paging::PteBits::kPresent);
		};
		const auto EntryBase4K = [](std::uint64_t entry) {
			return static_cast<std::uintptr_t>(entry & ~static_cast<std::uint64_t>(Rocinante::Memory::Paging::kPageOffsetMask));
		};

		if (dir3) {
			const std::uint64_t e3 = dir3->entries[idx_dir3];
			uart.puts("Paging bring-up: e3=");
			uart.write_dec_u64(e3);
			uart.putc('\n');
			if (IsWalkable(e3)) {
				auto* dir2 = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(EntryBase4K(e3));
				const std::uint64_t e2 = dir2 ? dir2->entries[idx_dir2] : 0;
				uart.puts("Paging bring-up: e2=");
				uart.write_dec_u64(e2);
				uart.putc('\n');
				if (dir2 && IsWalkable(e2)) {
					auto* dirl = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(EntryBase4K(e2));
					const std::uint64_t e1 = dirl ? dirl->entries[idx_dirl] : 0;
					uart.puts("Paging bring-up: e1=");
					uart.write_dec_u64(e1);
					uart.putc('\n');
					if (dirl && IsWalkable(e1)) {
						auto* pt = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(EntryBase4K(e1));
						const std::uint64_t ep = pt ? pt->entries[idx_pt] : 0;
						uart.puts("Paging bring-up: ep=");
						uart.write_dec_u64(ep);
						uart.putc('\n');
					}
				}
			}
		}
	};
	DumpPagingProbe(kernel_physical_base);
	if (kernel_physical_end > kernel_physical_base) {
		DumpPagingProbe(kernel_physical_end - 1);
	}

	// Bring-up self-check: confirm our MMIO pages are mapped before we
	// enable paging.
	//
	// Why:
	// After CRMD.PG=1 and CRMD.DA=0, all instruction fetches and data
	// accesses use mapped address translation. If we forgot to map the
	// UART MMIO page (or mapped it at the wrong virtual address), the
	// very next UART print can fault, and we lose our primary debugging
	// channel.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 5.2 (Address Translation Mode): when CRMD.DA=0 and
	//   CRMD.PG=1, translation is performed via the page tables.
	//
	// Leaf PTE inspection helper.
	//
	// Goal:
	// Confirm that our critical pages are not only mapped, but mapped with
	// the intended per-page attributes (cache mode + execute inhibit).
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - "Table entry format for common pages": leaf entries include cache
	//   mode (MAT) and execute inhibit (NX).
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

			auto* dir3 = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(root.root_physical_address);
			if (!dir3) return Rocinante::nullopt;

			const auto EntryIsWalkable = [](std::uint64_t entry) {
				return (entry & (Rocinante::Memory::Paging::PteBits::kValid | Rocinante::Memory::Paging::PteBits::kPresent)) ==
					(Rocinante::Memory::Paging::PteBits::kValid | Rocinante::Memory::Paging::PteBits::kPresent);
			};
			const auto EntryBase4K = [](std::uint64_t entry) {
				return static_cast<std::uintptr_t>(
					entry & ~static_cast<std::uint64_t>(Rocinante::Memory::Paging::kPageOffsetMask)
				);
			};

			const std::uint64_t e3 = dir3->entries[idx_dir3];
			if (!EntryIsWalkable(e3)) return Rocinante::nullopt;
			auto* dir2 = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(EntryBase4K(e3));
			if (!dir2) return Rocinante::nullopt;

			const std::uint64_t e2 = dir2->entries[idx_dir2];
			if (!EntryIsWalkable(e2)) return Rocinante::nullopt;
			auto* dirl = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(EntryBase4K(e2));
			if (!dirl) return Rocinante::nullopt;

			const std::uint64_t e1 = dirl->entries[idx_dirl];
			if (!EntryIsWalkable(e1)) return Rocinante::nullopt;
			auto* pt = reinterpret_cast<Rocinante::Memory::Paging::PageTablePage*>(EntryBase4K(e1));
			if (!pt) return Rocinante::nullopt;

			return Rocinante::Optional<std::uint64_t>(pt->entries[idx_pt]);
		};

	uart.puts("Paging bring-up: MMIO translation self-check\n");
	{
		const auto uart_translated =
			Rocinante::Memory::Paging::Translate(root, UART_BASE, address_bits);
		uart.puts("Paging bring-up:   Translate(UART_BASE)=");
		if (uart_translated.has_value()) {
			uart.write_dec_u64(uart_translated.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');
		if (!uart_translated.has_value() || uart_translated.value() != UART_BASE) {
			uart.puts("Paging bring-up: MMIO self-check FAILED (UART mapping)\n");
			Rocinante::Platform::Halt();
		}

		const auto uart_pte_or = ReadLeafPteEntry_Assuming4Level4KiB(UART_BASE);
		uart.puts("Paging bring-up:   UART pte=");
		if (uart_pte_or.has_value()) {
			uart.write_dec_u64(uart_pte_or.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');
		if (!uart_pte_or.has_value()) {
			uart.puts("Paging bring-up: MMIO self-check FAILED (UART PTE walk)\n");
			Rocinante::Platform::Halt();
		}
		{
			const std::uint64_t uart_pte = uart_pte_or.value();
			const std::uint64_t uart_cache_field =
				(uart_pte & Rocinante::Memory::Paging::PteBits::kCacheMask) >> Rocinante::Memory::Paging::PteBits::kCacheShift;
			const bool uart_nx = (uart_pte & Rocinante::Memory::Paging::PteBits::kNoExecute) != 0;
			uart.puts("Paging bring-up:   UART cache=");
			uart.write_dec_u64(uart_cache_field);
			uart.puts(" nx=");
			uart.puts(uart_nx ? "1" : "0");
			uart.putc('\n');

			const std::uint64_t expected_cache =
				static_cast<std::uint64_t>(Rocinante::Memory::Paging::CacheMode::StrongUncached);
			if (uart_cache_field != expected_cache || !uart_nx) {
				uart.puts("Paging bring-up: MMIO self-check FAILED (UART attributes)\n");
				Rocinante::Platform::Halt();
			}
		}

		const auto syscon_translated =
			Rocinante::Memory::Paging::Translate(root, kSysconBase, address_bits);
		uart.puts("Paging bring-up:   Translate(SysconBase)=");
		if (syscon_translated.has_value()) {
			uart.write_dec_u64(syscon_translated.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');
		if (!syscon_translated.has_value() || syscon_translated.value() != kSysconBase) {
			uart.puts("Paging bring-up: MMIO self-check FAILED (syscon mapping)\n");
			Rocinante::Platform::Halt();
		}

		const auto syscon_pte_or = ReadLeafPteEntry_Assuming4Level4KiB(kSysconBase);
		uart.puts("Paging bring-up:   syscon pte=");
		if (syscon_pte_or.has_value()) {
			uart.write_dec_u64(syscon_pte_or.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');
		if (!syscon_pte_or.has_value()) {
			uart.puts("Paging bring-up: MMIO self-check FAILED (syscon PTE walk)\n");
			Rocinante::Platform::Halt();
		}
		{
			const std::uint64_t syscon_pte = syscon_pte_or.value();
			const std::uint64_t syscon_cache_field =
				(syscon_pte & Rocinante::Memory::Paging::PteBits::kCacheMask) >> Rocinante::Memory::Paging::PteBits::kCacheShift;
			const bool syscon_nx = (syscon_pte & Rocinante::Memory::Paging::PteBits::kNoExecute) != 0;
			uart.puts("Paging bring-up:   syscon cache=");
			uart.write_dec_u64(syscon_cache_field);
			uart.puts(" nx=");
			uart.puts(syscon_nx ? "1" : "0");
			uart.putc('\n');

			const std::uint64_t expected_cache =
				static_cast<std::uint64_t>(Rocinante::Memory::Paging::CacheMode::StrongUncached);
			if (syscon_cache_field != expected_cache || !syscon_nx) {
				uart.puts("Paging bring-up: MMIO self-check FAILED (syscon attributes)\n");
				Rocinante::Platform::Halt();
			}
		}
	}
	uart.puts("Paging bring-up: MMIO translation self-check OK\n");

	// Bring-up self-check: confirm the physmap maps at least the root page
	// table page.
	//
	// Why:
	// Once paging is enabled (CRMD.DA=0, CRMD.PG=1), the kernel must not
	// dereference page-table pages by treating their physical address as a
	// pointer. Our paging code relies on the physmap to access those pages.
	// If the physmap window does not cover the root page-table page, page
	// table walks will fault immediately in mapped mode.
	//
	// Spec anchors (LoongArch-Vol1-EN.html):
	// - Section 5.2 (Address Translation Mode): when CRMD.DA=0 and CRMD.PG=1,
	//   translation uses the page-table mappings.
	// - "Table entry format for common pages": leaf entries include cache
	//   mode (MAT) and execute inhibit (NX).
	uart.puts("Paging bring-up: physmap translation self-check\n");
	if (physmap_size_bytes == 0) {
		uart.puts("Paging bring-up: physmap self-check skipped (no physmap window)\n");
	} else {
		const std::uintptr_t root_phys = root.root_physical_address;
		const std::uintptr_t physmap_physical_limit = physmap_physical_base + physmap_size_bytes;
		if (root_phys < physmap_physical_base || root_phys >= physmap_physical_limit) {
			uart.puts("Paging bring-up: physmap self-check FAILED (root_pt not covered)\n");
			Rocinante::Platform::Halt();
		}

		const std::uintptr_t physmap_root_virtual =
			Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(
				root_phys,
				address_bits.virtual_address_bits
			);

		const auto root_translated =
			Rocinante::Memory::Paging::Translate(root, physmap_root_virtual, address_bits);
		uart.puts("Paging bring-up:   Translate(physmap(root_pt))=");
		if (root_translated.has_value()) {
			uart.write_dec_u64(root_translated.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');
		if (!root_translated.has_value() || root_translated.value() != root_phys) {
			uart.puts("Paging bring-up: physmap self-check FAILED (Translate mismatch)\n");
			Rocinante::Platform::Halt();
		}

		const auto root_pte_or = ReadLeafPteEntry_Assuming4Level4KiB(physmap_root_virtual);
		uart.puts("Paging bring-up:   physmap(root_pt) pte=");
		if (root_pte_or.has_value()) {
			uart.write_dec_u64(root_pte_or.value());
		} else {
			uart.puts("<none>");
		}
		uart.putc('\n');
		if (!root_pte_or.has_value()) {
			uart.puts("Paging bring-up: physmap self-check FAILED (PTE walk)\n");
			Rocinante::Platform::Halt();
		}
		{
			const std::uint64_t root_pte = root_pte_or.value();
			const std::uint64_t root_cache_field =
				(root_pte & Rocinante::Memory::Paging::PteBits::kCacheMask) >> Rocinante::Memory::Paging::PteBits::kCacheShift;
			const bool root_nx = (root_pte & Rocinante::Memory::Paging::PteBits::kNoExecute) != 0;
			uart.puts("Paging bring-up:   physmap(root_pt) cache=");
			uart.write_dec_u64(root_cache_field);
			uart.puts(" nx=");
			uart.puts(root_nx ? "1" : "0");
			uart.putc('\n');

			const std::uint64_t expected_cache =
				static_cast<std::uint64_t>(Rocinante::Memory::Paging::CacheMode::CoherentCached);
			if (root_cache_field != expected_cache || !root_nx) {
				uart.puts("Paging bring-up: physmap self-check FAILED (attributes)\n");
				Rocinante::Platform::Halt();
			}
		}
	}
	uart.puts("Paging bring-up: physmap translation self-check OK\n");

	// Configure the page-walk CSRs and switch into mapped address
	// translation mode.
	//
	// Note:
	// The current LoongArch spec version used by this project describes
	// software-led TLB refill. We therefore enable paging even when the
	// CPU reports that a hardware page-table walker is not present.
	const bool enable_ptw = Rocinante::GetCPUCFG().SupportsPageTableWalker();
	const auto config_or = Rocinante::Memory::PagingHw::Make4KiBPageWalkerConfig(address_bits);
	if (!config_or.has_value()) {
		uart.puts("Paging bring-up: VALEN cannot be encoded in PWCL/PWCH for 4 KiB paging; skipping HW config.\n");
		return;
	}

	Rocinante::Memory::PagingHw::ConfigurePageTableWalker(root, config_or.value());
	uart.puts("Paging bring-up: configured PWCL/PWCH/PGD CSRs (CPUCFG.HPTW=");
	uart.puts(enable_ptw ? "on" : "off");
	uart.puts(")\n");

	uart.puts("Paging bring-up: invalidating TLB (INVTLB op=0)\n");
	Rocinante::Memory::PagingHw::InvalidateAllTlbEntries();
	uart.puts("Paging bring-up: enabling paging (CRMD.PG=1, CRMD.DA=0)\n");
	Rocinante::Memory::PagingHw::EnablePaging();
	uart.puts("Paging bring-up: paging enabled\n");

	// Switch to the higher-half stack (if mapped) and jump to a fresh
	// continuation function.
	//
	// Spec anchor:
	// - LoongArch Reference Manual Vol 1 (v1.10)
	//   - Section 7.5.6 (PGDH): higher half is selected when VA[VALEN-1]==1.
	{
		std::uintptr_t old_sp = 0;
		asm volatile("move %0, $sp" : "=r"(old_sp));
		const std::uintptr_t new_sp = (higher_half_stack_top != 0)
			? higher_half_stack_top
			: old_sp;
		if (higher_half_stack_top == 0) {
			uart.puts("Paging bring-up: higher-half stack not available; keeping low SP\n");
		}

		const std::uintptr_t continuation_low =
			reinterpret_cast<std::uintptr_t>(&PagingBringup_HigherHalfStackContinuation);
		const std::uintptr_t continuation_offset = continuation_low - kernel_physical_base;
		const std::uintptr_t continuation_high = kernel_higher_half_base + continuation_offset;

		uart.puts("Paging bring-up: switching SP from=");
		uart.write_dec_u64(old_sp);
		uart.puts(" to=");
		uart.write_dec_u64(new_sp);
		uart.putc('\n');

		uart.puts("Paging bring-up: jumping to higher-half stack continuation target=");
		uart.write_dec_u64(continuation_high);
		uart.putc('\n');

		asm volatile(
			"move $sp, %0\n"
			"jirl $zero, %1, 0\n"
			"break 0\n"
			:
			: "r"(new_sp), "r"(continuation_high)
			: "memory"
		);
		__builtin_unreachable();
	}
}

} // namespace Rocinante::Kernel

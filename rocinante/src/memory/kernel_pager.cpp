/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/memory/kernel_pager.h>

#include <src/memory/paging.h>
#include <src/memory/paging_hw.h>
#include <src/memory/paging_state.h>
#include <src/memory/pmm.h>

#include <src/sp/cpucfg.h>

#include <src/platform/console.h>
#include <src/sp/uart16550.h>
#include <src/trap/trap.h>

#include <cstdint>

namespace {

static Rocinante::Memory::KernelPager::LazyMappingRegion g_lazy_region{};
static bool g_installed = false;

static bool IsPageAligned(std::uintptr_t address) {
	return (address % Rocinante::Memory::Paging::kPageSizeBytes) == 0;
}

static bool IsValidLazyRegion(const Rocinante::Memory::KernelPager::LazyMappingRegion& region) {
	if (region.virtual_base == 0) return false;
	if (region.size_bytes == 0) return false;
	if (!IsPageAligned(region.virtual_base)) return false;
	if ((region.size_bytes % Rocinante::Memory::Paging::kPageSizeBytes) != 0) return false;
	const std::uintptr_t limit = region.virtual_base + static_cast<std::uintptr_t>(region.size_bytes);
	if (limit < region.virtual_base) return false;
	return true;
}

static bool LazyRegionContainsVirtualPageBase(std::uintptr_t virtual_page_base) {
	if (!IsValidLazyRegion(g_lazy_region)) return false;
	const std::uintptr_t limit = g_lazy_region.virtual_base + static_cast<std::uintptr_t>(g_lazy_region.size_bytes);
	return virtual_page_base >= g_lazy_region.virtual_base && virtual_page_base < limit;
}

static const char* PagingExceptionNameOrNull(std::uint64_t exception_code) {
	// LoongArch-Vol1-EN.html:
	// - Table 21 (exception encoding) and Section 5.4.3.1 (TLB-related exceptions).
	switch (exception_code) {
		case 0x1: return "PIL"; // page invalid for load
		case 0x2: return "PIS"; // page invalid for store
		case 0x3: return "PIF"; // page invalid for fetch
		case 0x4: return "PME"; // page modify
		case 0x5: return "PNR"; // page non-readable
		case 0x6: return "PNX"; // page non-executable
		case 0x7: return "PPI"; // page privilege illegal
		default: return nullptr;
	}
}

static Rocinante::Trap::PagingFaultResult KernelPagerPagingFaultObserver(
	Rocinante::TrapFrame& tf,
	const Rocinante::Trap::PagingFaultEvent& event
) {
	(void)tf;

	// Bring-up policy: only handle kernel-mode (PLV0) faults.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 5.4.3.1: paging exceptions are raised based on the current
	//   privilege level (CSR.CRMD.PLV) and the TLB entry PLV/RPLV.
	if (event.current_privilege_level != 0) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// We only implement demand mapping for page-invalid load/store.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 5.4.3.1: PIL/PIS are raised when the access hits a TLB entry with V=0.
	static constexpr std::uint64_t kExceptionCodePIL = 0x1;
	static constexpr std::uint64_t kExceptionCodePIS = 0x2;
	if (event.exception_code != kExceptionCodePIL && event.exception_code != kExceptionCodePIS) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Defensive recursion guard (bring-up, single CPU).
	static bool g_handling = false;
	if (g_handling) return Rocinante::Trap::PagingFaultResult::NotHandled;
	g_handling = true;

	const std::uintptr_t fault_virtual_page_base =
		static_cast<std::uintptr_t>(event.bad_virtual_address) &
		~static_cast<std::uintptr_t>(Rocinante::Memory::Paging::kPageOffsetMask);
	if (!LazyRegionContainsVirtualPageBase(fault_virtual_page_base)) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	const auto* paging_state = Rocinante::Memory::TryGetPagingState();

	// Address-width configuration for software page-table walking.
	//
	// Bring-up policy:
	// - Prefer the installed PagingState if present (it represents the kernel's
	//   chosen effective address widths).
	// - Fall back to CPUCFG-reported widths (VALEN/PALEN) otherwise.
	//
	// This keeps the pager usable in paging-hardware tests that enable paging
	// before higher-level memory subsystems install a PagingState.
	Rocinante::Memory::Paging::AddressSpaceBits address_bits{};
	if (paging_state) {
		address_bits = paging_state->address_bits;
	} else {
		address_bits = Rocinante::Memory::Paging::AddressSpaceBits{
			.virtual_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits()),
			.physical_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().PhysicalAddressBits()),
		};
	}

	// Map the missing page into the effective root for the faulting address.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 7.5.7 (PGD): CSR.PGD provides the effective root Base corresponding
	//   to the fault context (BADV when IsTLBR=0).
	const Rocinante::Memory::Paging::PageTableRoot root{
		.root_physical_address = static_cast<std::uintptr_t>(event.pgd_base),
	};

	// If the page is already mapped, this policy does not attempt to "repair" it.
	if (Rocinante::Memory::Paging::Translate(root, fault_virtual_page_base, address_bits).has_value()) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	const auto physical_page_or = pmm.AllocatePage();
	if (!physical_page_or.has_value()) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}
	const std::uintptr_t physical_page = physical_page_or.value();
	if (!IsPageAligned(physical_page)) {
		(void)pmm.FreePage(physical_page);
		g_handling = false;
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
		fault_virtual_page_base,
		physical_page,
		kPermissions,
		address_bits
	);
	if (!mapped) {
		(void)pmm.FreePage(physical_page);
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Ensure the retried instruction observes the updated page tables.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 4.2.4.7 (INVTLB) + Table 13:
	//   - op=0x5 clears G=0 entries matching {ASID, VA}.
	//   - op=0x2 clears all G=1 (global) entries.
	//
	// Policy (bring-up):
	// - Invalidate the faulting VA for the active ASID.
	// - This pager installs global (G=1) mappings, so also invalidate global
	//   entries system-wide.
	const std::uint16_t current_asid = Rocinante::Memory::PagingHw::GetAddressSpaceId();
	Rocinante::Memory::PagingHw::InvalidateNonGlobalTlbEntryForAsidAndVa(current_asid, fault_virtual_page_base);
	Rocinante::Memory::PagingHw::InvalidateGlobalTlbEntries();

	// Logging policy (bring-up): emit one concise line per mapped page.
	auto& uart = Rocinante::Platform::GetEarlyUart();
	uart.puts("Kernel pager: mapped lazy page; exc=");
	if (const char* name = PagingExceptionNameOrNull(event.exception_code)) {
		uart.puts(name);
	} else {
		uart.puts("<unknown>");
	}
	uart.puts(" badv=");
	uart.write_hex_u64(event.bad_virtual_address);
	uart.puts(" va_page=");
	uart.write_hex_u64(fault_virtual_page_base);
	uart.puts(" pa_page=");
	uart.write_hex_u64(physical_page);
	uart.putc('\n');

	g_handling = false;

	// Return Handled without advancing ERA.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 4.2.6.1 (ERTN): for general exceptions, execution resumes at CSR.ERA.
	// If we repair the mapping and keep ERA unchanged, the faulting instruction
	// should be retried.
	return Rocinante::Trap::PagingFaultResult::Handled;
}

} // namespace

namespace Rocinante::Memory::KernelPager {

void ConfigureLazyMappingRegion(LazyMappingRegion region) {
	// Note: invalid input disables the pager's mapping policy.
	g_lazy_region = region;
}

void Install() {
	Rocinante::Trap::SetPagingFaultObserver(&KernelPagerPagingFaultObserver);
	g_installed = true;

	// Log install once.
	auto& uart = Rocinante::Platform::GetEarlyUart();
	uart.puts("Kernel pager: installed");
	if (IsValidLazyRegion(g_lazy_region)) {
		uart.puts(" lazy_region=[");
		uart.write_hex_u64(g_lazy_region.virtual_base);
		uart.puts(", ");
		uart.write_hex_u64(g_lazy_region.virtual_base + static_cast<std::uintptr_t>(g_lazy_region.size_bytes));
		uart.puts(")");
	} else {
		uart.puts(" (no lazy region configured)");
	}
	uart.putc('\n');
}

} // namespace Rocinante::Memory::KernelPager

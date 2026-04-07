/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/memory/vmm_pager.h>

#include <src/memory/paging.h>
#include <src/memory/paging_hw.h>
#include <src/memory/paging_state.h>
#include <src/memory/pmm.h>
#include <src/memory/vm_object.h>

#include <src/sp/cpucfg.h>

#include <src/platform/console.h>
#include <src/sp/uart16550.h>

#include <cstddef>
#include <cstdint>

namespace {

const Rocinante::Memory::VirtualMemoryAreaSet* g_kernel_vmas = nullptr;

bool IsKernelMode(const Rocinante::Trap::PagingFaultEvent& event) {
	return event.current_privilege_level == 0;
}

bool IsPageInvalidLoadOrStore(std::uint64_t exception_code) {
	// LoongArch-Vol1-EN.html, Table 21 (Table of exception encoding).
	static constexpr std::uint64_t kExceptionCodePil = 0x1; // page invalid for load
	static constexpr std::uint64_t kExceptionCodePis = 0x2; // page invalid for store
	return exception_code == kExceptionCodePil || exception_code == kExceptionCodePis;
}

std::uintptr_t VirtualPageBase(std::uintptr_t virtual_address) {
	return virtual_address & ~static_cast<std::uintptr_t>(Rocinante::Memory::Paging::kPageOffsetMask);
}

bool AccessIsPermittedByVma(const Rocinante::Memory::VirtualMemoryArea& vma, Rocinante::Trap::PagingAccessType access) {
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::ExecutePermissions;

	switch (access) {
		case Rocinante::Trap::PagingAccessType::Load:
			return true;
		case Rocinante::Trap::PagingAccessType::Store:
			return vma.permissions.access == AccessPermissions::ReadWrite;
		case Rocinante::Trap::PagingAccessType::Fetch:
			return vma.permissions.execute == ExecutePermissions::Executable;
		case Rocinante::Trap::PagingAccessType::Unknown:
			return false;
	}
	return false;
}

} // namespace

namespace Rocinante::Memory::VmmPager {

void ConfigureKernelVirtualMemoryAreas(const VirtualMemoryAreaSet* areas) {
	g_kernel_vmas = areas;
}

Rocinante::Trap::PagingFaultResult PagingFaultObserver(
	Rocinante::TrapFrame* tf,
	const Rocinante::Trap::PagingFaultEvent& event
) {
	(void)tf;

	// Bring-up policy: only handle kernel-mode (PLV0) faults.
	if (!IsKernelMode(event)) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Bring-up policy: only handle page-invalid load/store faults.
	//
	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 5.4.3.1 (TLB-related Exceptions): PIL/PIS are raised when the access
	//   finds a matching TLB entry with V=0.
	// - Table 21 (Table of exception encoding): PIL=0x1, PIS=0x2.
	if (!IsPageInvalidLoadOrStore(event.exception_code)) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	if (g_kernel_vmas == nullptr) {
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Defensive recursion guard (bring-up, single CPU).
	static bool g_handling = false;
	if (g_handling) return Rocinante::Trap::PagingFaultResult::NotHandled;
	g_handling = true;

	const std::uintptr_t fault_virtual_page_base =
		VirtualPageBase(static_cast<std::uintptr_t>(event.bad_virtual_address));

	const VirtualMemoryArea* vma = g_kernel_vmas->FindVmaForAddress(fault_virtual_page_base);
	if (!vma) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	if (vma->backing_type != VirtualMemoryArea::BackingType::Anonymous) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Bring-up flaw: the VMA does not yet support non-anonymous backing types.
	if (vma->anonymous_object == nullptr) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	if (!AccessIsPermittedByVma(*vma, event.access_type)) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	// Address-width configuration for software page-table walking.
	//
	// Bring-up policy:
	// - Prefer the installed PagingState if present (it represents the kernel's
	//   chosen effective address widths).
	// - Fall back to CPUCFG-reported widths (VALEN/PALEN) otherwise.
	//
	// This keeps the observer usable in paging-hardware tests that enable paging
	// before higher-level memory subsystems install a PagingState.
	Rocinante::Memory::Paging::AddressSpaceBits address_bits{};
	if (const auto* paging_state = Rocinante::Memory::TryGetPagingState()) {
		address_bits = paging_state->address_bits;
	} else {
		address_bits = Rocinante::Memory::Paging::AddressSpaceBits{
			.virtual_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().VirtualAddressBits()),
			.physical_address_bits = static_cast<std::uint8_t>(Rocinante::GetCPUCFG().PhysicalAddressBits()),
		};
	}

	// Spec anchor (LoongArch-Vol1-EN.html):
	// - Section 7.5.7 (PGD): CSR.PGD provides the effective root Base corresponding
	//   to the fault context (BADV when IsTLBR=0).
	const Rocinante::Memory::Paging::PageTableRoot root{
		.root_physical_address = static_cast<std::uintptr_t>(event.pgd_base),
	};

	// If the page is already mapped, this observer does not attempt to "repair" it.
	if (Rocinante::Memory::Paging::Translate(root, fault_virtual_page_base, address_bits).has_value()) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	if (fault_virtual_page_base < vma->virtual_base) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}
	const std::uintptr_t offset_bytes = fault_virtual_page_base - vma->virtual_base;
	if ((offset_bytes % Rocinante::Memory::Paging::kPageSizeBytes) != 0) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}
	const auto page_offset = static_cast<std::size_t>(
		offset_bytes / Rocinante::Memory::Paging::kPageSizeBytes);

	auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
	const auto frame_or = vma->anonymous_object->GetOrCreateFrameForPageOffset(&pmm, page_offset);
	if (!frame_or.has_value()) {
		g_handling = false;
		return Rocinante::Trap::PagingFaultResult::NotHandled;
	}

	const std::uintptr_t physical_page_base = frame_or.value().physical_page_base;

	const bool mapped = Rocinante::Memory::Paging::MapPage4KiB(
		&pmm,
		root,
		fault_virtual_page_base,
		physical_page_base,
		vma->permissions,
		address_bits
	);
	if (!mapped) {
		// Explicit flaw:
		// The anonymous VM object currently commits newly allocated frames into its
		// internal directory as part of GetOrCreateFrameForPageOffset(). If we fail
		// to map after creating a new frame, we have no removal API to roll back the
		// ownership record. For bring-up, we treat this as a hard failure to handle.
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
	// - Invalidate the faulting VA for the current ASID.
	// - If we installed a global mapping (G=1), also invalidate global entries
	//   system-wide, since global translations do not participate in ASID matching.
	const std::uint16_t current_asid = Rocinante::Memory::PagingHw::GetAddressSpaceId();
	Rocinante::Memory::PagingHw::InvalidateNonGlobalTlbEntryForAsidAndVa(current_asid, fault_virtual_page_base);
	if (vma->permissions.global) {
		Rocinante::Memory::PagingHw::InvalidateGlobalTlbEntries();
	}

	// Logging policy (bring-up): emit one concise line per mapped page.
	auto& uart = Rocinante::Platform::GetEarlyUart();
	uart.puts("VMM pager: mapped anonymous page; badv=");
	uart.write_hex_u64(event.bad_virtual_address);
	uart.puts(" va_page=");
	uart.write_hex_u64(fault_virtual_page_base);
	uart.puts(" pa_page=");
	uart.write_hex_u64(physical_page_base);
	uart.puts(" page_offset=");
	uart.write_dec_u64(page_offset);
	uart.putc('\n');

	g_handling = false;

	// Return Handled without advancing ERA: the faulting instruction retries.
	return Rocinante::Trap::PagingFaultResult::Handled;
}

} // namespace Rocinante::Memory::VmmPager

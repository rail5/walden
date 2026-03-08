/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include "paging.h"

namespace Rocinante::Memory::PagingHw {

/**
 * @brief LoongArch paging CSR bring-up helpers.
 *
 * Scope:
 * - Program CSRs needed for the hardware page-table walker.
 * - Optionally switch the CPU into paging mode (CRMD.DA/CRMD.PG).
 *
 * Explicit flaws / limitations:
 * - This code currently only supports 4 KiB pages.
 * - Page-walker configuration (PWCL/PWCH) is derived from CPUCFG-reported VALEN.
 *   The PWCL/PWCH field layout encodes up to 5 index levels (PT + 4 directories),
 *   i.e. VALEN up to (PAGE_SHIFT + 5*9) == 57 for 4 KiB pages.
 * - It assumes the page table entry format produced by Rocinante::Memory::Paging matches
 *   what the LoongArch hardware page-table walker expects. That assumption must be
 *   validated before enabling paging by default.
 */

struct PageWalkerConfig final {
	std::uint64_t pwcl;
	std::uint64_t pwch;
};

/**
 * @brief Computes PWCL/PWCH for 4 KiB paging from CPUCFG-reported VALEN.
 *
 * Returns empty if VALEN cannot be represented with the PWCL/PWCH fields for
 * 4 KiB pages (e.g. requiring more than 5 index levels).
 */
Rocinante::Optional<PageWalkerConfig> Make4KiBPageWalkerConfig(Paging::AddressSpaceBits address_bits);

/**
 * @brief Programs the hardware page-table walker CSRs for the supplied root.
 *
 * This writes:
 * - CSR.PWCL, CSR.PWCH
 * - CSR.PGDL, CSR.PGDH (both set to the same root for early bring-up)
 */
void ConfigurePageTableWalker(const Paging::PageTableRoot& root, PageWalkerConfig config);

/**
 * @brief Programs the hardware page-table walker CSRs for separate low-half and high-half roots.
 *
 * This writes:
 * - CSR.PWCL, CSR.PWCH
 * - CSR.PGDL (low-half root)
 * - CSR.PGDH (high-half root)
 */
void ConfigurePageTableWalkerRoots(
	const Paging::PageTableRoot& low_half_root,
	const Paging::PageTableRoot& high_half_root,
	PageWalkerConfig config);

/**
 * @brief Sets CSR.ASID.ASID to the supplied address-space identifier.
 *
 * Spec anchor (LoongArch-Vol1-EN.html):
 * - Vol.1 Section 7.5.4 (ASID), Table 38:
 *   - CSR.ASID.ASID is bits [9:0]
 *   - CSR.ASID.ASIDBITS is bits [23:16]
 */
void SetAddressSpaceId(std::uint16_t address_space_id);

/**
 * @brief Returns the current address-space identifier (ASID) from CSR.ASID.ASID.
 *
 * Spec anchor (LoongArch-Vol1-EN.html):
 * - Vol.1 Section 7.5.4 (ASID), Table 38:
 *   - CSR.ASID.ASID is bits [9:0]
 */
std::uint16_t GetAddressSpaceId();

/**
 * @brief Sets CSR.PGDL.Base (low-half root page directory base).
 *
 * Spec anchor (LoongArch-Vol1-EN.html):
 * - Vol.1 Section 7.5.5 (PGDL): Base is 4 KiB aligned.
 */
void SetLowHalfRootPageDirectoryBase(const Paging::PageTableRoot& low_half_root);

/**
 * @brief Activates a low-half address space.
 *
 * Policy (bring-up):
 * - Programs CSR.ASID.ASID and CSR.PGDL.Base.
 * - Invalidates non-global TLB entries for the supplied ASID to avoid stale translations.
 *
 * Explicit flaw:
 * - This does not invalidate global entries and does not provide a per-page invalidation policy yet.
 */
void ActivateLowHalfAddressSpace(const Paging::PageTableRoot& low_half_root, std::uint16_t address_space_id);

/**
 * @brief Invalidates all TLB entries.
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB)
 *   op=0x0: "Clear all page table entries"
 */
void InvalidateAllTlbEntries();

/**
 * @brief Invalidates all global (G=1) TLB entries.
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB), Table 13
 *   op=0x2: "Clears all G=1 page table entries."
 */
void InvalidateGlobalTlbEntries();

/**
 * @brief Invalidates all non-global (G=0) TLB entries (all ASIDs).
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB), Table 13
 *   op=0x3: "Clears all page table entries with G=0."
 */
void InvalidateNonGlobalTlbEntries();

/**
 * @brief Invalidates non-global (G=0) TLB entries for one ASID.
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB), Table 13
 *   op=0x4: "Clears all page table entries with G=0 and ASID equal to the ASID specified in the register."
 */
void InvalidateNonGlobalTlbEntriesForAsid(std::uint16_t address_space_id);

/**
 * @brief Invalidates one non-global (G=0) TLB entry for a specific ASID+VA.
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB), Table 13
 *   op=0x5: "Clear all page table entries with G=0 and ASID equal to the register specified ASID,
 *            and VA equal to the register specified VA."
 */
void InvalidateNonGlobalTlbEntryForAsidAndVa(std::uint16_t address_space_id, std::uintptr_t virtual_address);

/**
 * @brief Invalidates a VA mapping including global entries (G=1) and the specified ASID.
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB), Table 13
 *   op=0x6: "Clear all page table entries where G=1 or ASID is equal to the ASID specified in the register
 *            and VA is equal to the VA specified in the register."
 */
void InvalidateGlobalOrAsidTlbEntryForVa(std::uint16_t address_space_id, std::uintptr_t virtual_address);

/**
 * @brief Switches the CPU from direct-address mode to paging mode.
 *
 * Policy:
 * - Sets CRMD.PG=1 and clears CRMD.DA=0.
 *
 * WARNING: calling this with incorrect page tables or without an identity mapping
 * for the current PC/stack will likely trap or hang.
 */
void EnablePaging();

} // namespace Rocinante::Memory::PagingHw

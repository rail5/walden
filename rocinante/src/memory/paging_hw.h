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
 * @brief Invalidates all TLB entries.
 *
 * Spec:
 * - LoongArch-Vol1-EN.html, Section 4.2.4.7 (INVTLB)
 *   op=0x0: "Clear all page table entries"
 */
void InvalidateAllTlbEntries();

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

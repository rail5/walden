/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "paging_hw.h"

namespace {

// CSR numbering matches the LoongArch privileged architecture spec.
namespace Csr {
	constexpr std::uint32_t kCurrentModeInformation = 0x0; // CSR.CRMD
	constexpr std::uint32_t kPgdLow = 0x19;               // CSR.PGDL
	constexpr std::uint32_t kPgdHigh = 0x1a;              // CSR.PGDH
	constexpr std::uint32_t kPageWalkControlLow = 0x1c;   // CSR.PWCL
	constexpr std::uint32_t kPageWalkControlHigh = 0x1d;  // CSR.PWCH
}

namespace CurrentModeInformation {
	// CRMD.PG (bit 4): enable paging.
	constexpr std::uint64_t kPagingEnable = (1ull << 4);
	// CRMD.DA (bit 3): direct addressing enable.
	constexpr std::uint64_t kDirectAddressingEnable = (1ull << 3);
}

static inline std::uint64_t ReadCsr(std::uint32_t csr) {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(csr));
	return value;
}

static inline void WriteCsr(std::uint32_t csr, std::uint64_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(csr));
}

} // namespace

namespace Rocinante::Memory::PagingHw {

Rocinante::Optional<PageWalkerConfig> Make4KiBPageWalkerConfig(Paging::AddressSpaceBits address_bits) {
	// PWCL/PWCH define the multi-level page-table shape used by page walking.
	//
	// The LoongArch privileged spec defines these fields as index start positions
	// (e.g. PTbase) and index widths (PTwidth, Dirl_width, ...).
	//
	// For 4 KiB pages, the page offset is 12 bits. The remaining (VALEN-12) bits
	// are split across page-table indices from lowest to highest level.
	if (address_bits.virtual_address_bits <= Paging::kPageShiftBits) return {};

	constexpr std::uint8_t kMaxIndexLevels = 5; // PT + Dirl + Dir2 + Dir3 + Dir4
	std::uint8_t widths[kMaxIndexLevels]{};
	std::uint8_t level_count = 0;
	std::uint32_t remaining = static_cast<std::uint32_t>(address_bits.virtual_address_bits) - Paging::kPageShiftBits;
	while (remaining > 0) {
		if (level_count >= kMaxIndexLevels) {
			return {};
		}
		const std::uint8_t w = (remaining >= Paging::kIndexBitsPerLevel)
			? static_cast<std::uint8_t>(Paging::kIndexBitsPerLevel)
			: static_cast<std::uint8_t>(remaining);
		widths[level_count++] = w;
		remaining -= w;
	}

	// Compute the index start bit for each level.
	std::uint8_t bases[kMaxIndexLevels]{};
	bases[0] = static_cast<std::uint8_t>(Paging::kPageShiftBits);
	for (std::uint8_t i = 1; i < level_count; i++) {
		bases[i] = static_cast<std::uint8_t>(bases[i - 1] + widths[i - 1]);
	}

	// Pack PWCL.
	// Bits:
	// - [ 4: 0] PTbase
	// - [ 9: 5] PTwidth
	// - [14:10] Dirl_base
	// - [19:15] Dirl_width
	// - [24:20] Dir2_base
	// - [29:25] Dir2_width
	// - [31:30] PTEWidth (0 => 64-bit entries)
	std::uint64_t pwcl = 0;
	pwcl |= static_cast<std::uint64_t>(bases[0] & 0x1Fu);
	pwcl |= static_cast<std::uint64_t>(widths[0] & 0x1Fu) << 5;
	if (level_count >= 2) {
		pwcl |= static_cast<std::uint64_t>(bases[1] & 0x1Fu) << 10;
		pwcl |= static_cast<std::uint64_t>(widths[1] & 0x1Fu) << 15;
	}
	if (level_count >= 3) {
		pwcl |= static_cast<std::uint64_t>(bases[2] & 0x1Fu) << 20;
		pwcl |= static_cast<std::uint64_t>(widths[2] & 0x1Fu) << 25;
	}
	// PTEWidth=0 (64-bit entries).

	// Pack PWCH.
	// Bits:
	// - [ 5: 0] Dir3_base
	// - [11: 6] Dir3_width
	// - [17:12] Dir4_base
	// - [23:18] Dir4_width
	// - [31:24] reserved (R0) => must remain 0
	std::uint64_t pwch = 0;
	if (level_count >= 4) {
		pwch |= static_cast<std::uint64_t>(bases[3] & 0x3Fu);
		pwch |= static_cast<std::uint64_t>(widths[3] & 0x3Fu) << 6;
	}
	if (level_count >= 5) {
		pwch |= static_cast<std::uint64_t>(bases[4] & 0x3Fu) << 12;
		pwch |= static_cast<std::uint64_t>(widths[4] & 0x3Fu) << 18;
	}

	return PageWalkerConfig{.pwcl = pwcl, .pwch = pwch};
}

void ConfigurePageTableWalker(const Paging::PageTableRoot& root, PageWalkerConfig config) {
	WriteCsr(Csr::kPageWalkControlLow, config.pwcl);
	WriteCsr(Csr::kPageWalkControlHigh, config.pwch);

	// Early bring-up: use the same root for both halves.
	//
	// Flaw / bring-up gap:
	// We do not yet build a full higher-half/physmap layout with distinct roots
	// (or separate roots for different address ranges). That can be added once
	// paging is enabled by default and the virtual layout is finalized.
	WriteCsr(Csr::kPgdHigh, static_cast<std::uint64_t>(root.root_physical_address));
	WriteCsr(Csr::kPgdLow, static_cast<std::uint64_t>(root.root_physical_address));
}

void EnablePaging() {
	std::uint64_t crmd = ReadCsr(Csr::kCurrentModeInformation);
	crmd |= CurrentModeInformation::kPagingEnable;
	crmd &= ~CurrentModeInformation::kDirectAddressingEnable;
	WriteCsr(Csr::kCurrentModeInformation, crmd);
}

} // namespace Rocinante::Memory::PagingHw

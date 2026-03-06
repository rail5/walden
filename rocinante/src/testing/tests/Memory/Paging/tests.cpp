/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpucfg.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/pmm.h>
#include <src/memory/paging.h>
#include <src/memory/virtual_layout.h>

#include <cstddef>
#include <cstdint>

extern "C" char _start;
extern "C" char _end;

namespace Rocinante::Testing {

namespace {

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

} // namespace

void TestEntry_Paging_MapTranslateUnmap(TestContext* ctx) {
	Test_Paging_MapTranslateUnmap(ctx);
}

void TestEntry_Paging_RespectsVALENAndPALEN(TestContext* ctx) {
	Test_Paging_RespectsVALENAndPALEN(ctx);
}

void TestEntry_Paging_Physmap_MapsRootPageTableAndAttributes(TestContext* ctx) {
	Test_Paging_Physmap_MapsRootPageTableAndAttributes(ctx);
}

} // namespace Rocinante::Testing

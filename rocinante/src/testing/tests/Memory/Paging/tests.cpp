/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpucfg.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/address_space.h>
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

	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(&pmm, root.value(), kVirtualPageBase));
	const auto translated_after_unmap = Translate(root.value(), kVirtualPageBase);
	ROCINANTE_EXPECT_TRUE(ctx, !translated_after_unmap.has_value());
}

static void Test_Paging_MapCount_TracksLeafMappings(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::MapPage4KiB;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::UnmapPage4KiB;

	// Regression guard:
	// Map/unmap must update per-frame map_count exactly once per leaf mapping.
	// Alias mappings must be counted correctly.

	static constexpr std::uintptr_t kUsableBase = 0x00100000;
	static constexpr std::size_t kUsableSizeBytes = 128 * PhysicalMemoryManager::kPageSizeBytes;

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
	if (!root.has_value()) return;

	const auto backing_page = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, backing_page.has_value());
	if (!backing_page.has_value()) return;
	const std::uintptr_t physical_page_base = backing_page.value();

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	const auto count0 = pmm.MapCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, count0.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, count0.value(), 0);

	static constexpr std::uintptr_t kVirtualPageBase0 = 0x0000000000100000ull;
	static constexpr std::uintptr_t kVirtualPageBase1 = 0x0000000000200000ull;

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root.value(), kVirtualPageBase0, physical_page_base, permissions));
	const auto count1 = pmm.MapCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, count1.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, count1.value(), 1);

	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root.value(), kVirtualPageBase1, physical_page_base, permissions));
	const auto count2 = pmm.MapCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, count2.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, count2.value(), 2);

	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(&pmm, root.value(), kVirtualPageBase0));
	const auto count_after_unmap0 = pmm.MapCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, count_after_unmap0.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, count_after_unmap0.value(), 1);

	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(&pmm, root.value(), kVirtualPageBase1));
	const auto count_after_unmap1 = pmm.MapCountForPhysical(physical_page_base);
	ROCINANTE_EXPECT_TRUE(ctx, count_after_unmap1.has_value());
	ROCINANTE_EXPECT_EQ_U64(ctx, count_after_unmap1.value(), 0);

	ROCINANTE_EXPECT_TRUE(ctx, pmm.FreePage(physical_page_base));
}

static void Test_Paging_UnmapReclaimsIntermediateTables(TestContext* ctx) {
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
	using Rocinante::Memory::Paging::UnmapPage4KiB;

	// Regression guard:
	// Unmapping the last page in a subtree should reclaim empty intermediate
	// page-table pages back to the PMM.

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

	const auto root_or = AllocateRootPageTable(&pmm);
	ROCINANTE_EXPECT_TRUE(ctx, root_or.has_value());
	if (!root_or.has_value()) return;

	const auto backing_page_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, backing_page_or.has_value());
	if (!backing_page_or.has_value()) return;
	const std::uintptr_t backing_physical = backing_page_or.value();

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	// Use a fixed, smaller address-width configuration to make level-count
	// deterministic for this test.
	const AddressSpaceBits bits{.virtual_address_bits = 39, .physical_address_bits = 44};

	static constexpr std::uintptr_t kVirtualPageBase0 = 0x0000000000100000ull;
	static constexpr std::uintptr_t kVirtualPageBase1 = kVirtualPageBase0 + PhysicalMemoryManager::kPageSizeBytes;

	const std::size_t free_before_map = pmm.FreePages();
	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root_or.value(), kVirtualPageBase0, backing_physical, permissions, bits));
	ROCINANTE_EXPECT_TRUE(ctx, MapPage4KiB(&pmm, root_or.value(), kVirtualPageBase1, backing_physical, permissions, bits));
	const std::size_t free_after_map = pmm.FreePages();
	ROCINANTE_EXPECT_TRUE(ctx, free_after_map <= free_before_map);

	// Unmapping one of two pages in the same leaf table should not reclaim the tables.
	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(&pmm, root_or.value(), kVirtualPageBase0, bits));
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_after_map);

	// Unmapping the last page should reclaim the now-empty subtree.
	ROCINANTE_EXPECT_TRUE(ctx, UnmapPage4KiB(&pmm, root_or.value(), kVirtualPageBase1, bits));
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before_map);

	ROCINANTE_EXPECT_TRUE(ctx, pmm.FreePage(backing_physical));
}

static void Test_AddressSpace_DestroyPageTables_FreesRootAndSubtables(TestContext* ctx) {
	using Rocinante::Memory::AddressSpace;
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::PagePermissions;

	// Regression guard:
	// Destroying an address space should reclaim the root page-table page, not
	// only intermediate tables reclaimed by per-page unmap.

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

	const AddressSpaceBits bits{.virtual_address_bits = 39, .physical_address_bits = 44};
	static constexpr std::uint16_t kAsid = 1;

	const std::size_t free_before_create = pmm.FreePages();
	auto as_or = AddressSpace::Create(&pmm, bits, kAsid);
	ROCINANTE_EXPECT_TRUE(ctx, as_or.has_value());
	if (!as_or.has_value()) return;
	AddressSpace address_space = as_or.value();
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_before_create - 1);

	const auto backing_page_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, backing_page_or.has_value());
	if (!backing_page_or.has_value()) return;
	const std::uintptr_t backing_physical = backing_page_or.value();

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	static constexpr std::uintptr_t kVirtualPageBase = 0x0000000000200000ull;
	const std::size_t free_before_map = pmm.FreePages();
	ROCINANTE_EXPECT_TRUE(ctx, address_space.MapPage4KiB(&pmm, kVirtualPageBase, backing_physical, permissions));
	const std::size_t free_after_map = pmm.FreePages();
	ROCINANTE_EXPECT_TRUE(ctx, free_after_map <= free_before_map);

	// For VALEN=39 with 4 KiB pages, the software paging builder uses 3 levels:
	// root + one directory + one leaf table.
	static constexpr std::size_t kExpectedPageTablePagesFreed = 3;
	ROCINANTE_EXPECT_TRUE(ctx, address_space.DestroyPageTables(&pmm));
	ROCINANTE_EXPECT_EQ_U64(ctx, pmm.FreePages(), free_after_map + kExpectedPageTablePagesFreed);

	ROCINANTE_EXPECT_TRUE(ctx, pmm.FreePage(backing_physical));
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
	const std::uintptr_t physmap_max_physical_limit =
		Rocinante::Memory::VirtualLayout::PhysMapMaxPhysicalAddressExclusive(address_bits.virtual_address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, physmap_max_physical_limit != 0);
	ROCINANTE_EXPECT_TRUE(ctx, (kUsableBase + kUsableSizeBytes) <= physmap_max_physical_limit);

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
	struct WalkResult final {
		std::uint64_t e3;
		std::uint64_t e2;
		std::uint64_t e1;
		std::uint64_t leaf;
	};

	const auto WalkToLeafPte_Assuming4Level4KiB =
		[&](std::uintptr_t probe_va) -> Rocinante::Optional<WalkResult> {
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

			return Rocinante::Optional<WalkResult>(WalkResult{
				.e3 = e3,
				.e2 = e2,
				.e1 = e1,
				.leaf = pt->entries[idx_pt],
			});
		};

	const auto walk_or = WalkToLeafPte_Assuming4Level4KiB(physmap_root_virtual);
	ROCINANTE_EXPECT_TRUE(ctx, walk_or.has_value());
	if (!walk_or.has_value()) return;
	const WalkResult walk = walk_or.value();
	const std::uint64_t pte = walk.leaf;

	// Intermediate directory entries must be walkable but must not claim "global".
	// In the privileged spec, bit 6 is used as a huge-page indicator in directory entries.
	ROCINANTE_EXPECT_TRUE(ctx, (walk.e3 & (PteBits::kValid | PteBits::kPresent)) == (PteBits::kValid | PteBits::kPresent));
	ROCINANTE_EXPECT_TRUE(ctx, (walk.e2 & (PteBits::kValid | PteBits::kPresent)) == (PteBits::kValid | PteBits::kPresent));
	ROCINANTE_EXPECT_TRUE(ctx, (walk.e1 & (PteBits::kValid | PteBits::kPresent)) == (PteBits::kValid | PteBits::kPresent));
	ROCINANTE_EXPECT_EQ_U64(ctx, (walk.e3 & PteBits::kGlobal), 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, (walk.e2 & PteBits::kGlobal), 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, (walk.e1 & PteBits::kGlobal), 0);

	// Reserved bits in the TLBELO low-order layout (11:9) should not be set in our
	// in-memory entries.
	static constexpr std::uint64_t kReservedBits11To9 = (0x7ull << 9);
	ROCINANTE_EXPECT_EQ_U64(ctx, (walk.e3 & kReservedBits11To9), 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, (walk.e2 & kReservedBits11To9), 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, (walk.e1 & kReservedBits11To9), 0);
	ROCINANTE_EXPECT_EQ_U64(ctx, (pte & kReservedBits11To9), 0);

	const std::uint64_t cache_field = (pte & PteBits::kCacheMask) >> PteBits::kCacheShift;
	const bool nx = (pte & PteBits::kNoExecute) != 0;
	const bool v = (pte & PteBits::kValid) != 0;
	const bool p = (pte & PteBits::kPresent) != 0;
	const bool w = (pte & PteBits::kWrite) != 0;
	const bool d = (pte & PteBits::kDirty) != 0;
	const bool g = (pte & PteBits::kGlobal) != 0;
	ROCINANTE_EXPECT_TRUE(ctx, nx);
	ROCINANTE_EXPECT_TRUE(ctx, v);
	ROCINANTE_EXPECT_TRUE(ctx, p);
	ROCINANTE_EXPECT_TRUE(ctx, w);
	ROCINANTE_EXPECT_TRUE(ctx, d);
	ROCINANTE_EXPECT_TRUE(ctx, g);
	ROCINANTE_EXPECT_EQ_U64(ctx, cache_field, static_cast<std::uint64_t>(CacheMode::CoherentCached));
}

} // namespace

void TestEntry_Paging_MapTranslateUnmap(TestContext* ctx) {
	Test_Paging_MapTranslateUnmap(ctx);
}

void TestEntry_Paging_MapCount_TracksLeafMappings(TestContext* ctx) {
	Test_Paging_MapCount_TracksLeafMappings(ctx);
}

void TestEntry_Paging_UnmapReclaimsIntermediateTables(TestContext* ctx) {
	Test_Paging_UnmapReclaimsIntermediateTables(ctx);
}

void TestEntry_AddressSpace_DestroyPageTables_FreesRootAndSubtables(TestContext* ctx) {
	Test_AddressSpace_DestroyPageTables_FreesRootAndSubtables(ctx);
}

void TestEntry_Paging_RespectsVALENAndPALEN(TestContext* ctx) {
	Test_Paging_RespectsVALENAndPALEN(ctx);
}

void TestEntry_Paging_Physmap_MapsRootPageTableAndAttributes(TestContext* ctx) {
	Test_Paging_Physmap_MapsRootPageTableAndAttributes(ctx);
}

} // namespace Rocinante::Testing

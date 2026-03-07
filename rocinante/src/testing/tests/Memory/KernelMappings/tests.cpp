/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpucfg.h>

#include <src/memory/boot_memory_map.h>
#include <src/memory/kernel_mappings.h>
#include <src/memory/kernel_va_allocator.h>
#include <src/memory/pmm.h>
#include <src/memory/paging.h>

#include <cstddef>
#include <cstdint>

extern "C" char _start;
extern "C" char _end;

namespace Rocinante::Testing {

namespace {

static void Test_KernelMappings_MapTranslateUnmapAndFree(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::KernelVirtualAddressAllocator;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::KernelMappings::MapPhysicalRange4KiB;
	using Rocinante::Memory::KernelMappings::UnmapAndFree4KiB;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::Translate;

	auto& cpucfg = Rocinante::GetCPUCFG();
	const AddressSpaceBits address_bits{
		.virtual_address_bits = static_cast<std::uint8_t>(cpucfg.VirtualAddressBits()),
		.physical_address_bits = static_cast<std::uint8_t>(cpucfg.PhysicalAddressBits()),
	};
	ROCINANTE_EXPECT_TRUE(ctx, address_bits.virtual_address_bits > 0);
	ROCINANTE_EXPECT_TRUE(ctx, address_bits.virtual_address_bits < 64);

	static constexpr std::uintptr_t kUsableBase = 0x01000000; // 16 MiB
	static constexpr std::size_t kUsableSizeBytes = 8u * 1024u * 1024u; // 8 MiB

	const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
	const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);
	ROCINANTE_EXPECT_TRUE(ctx, kernel_physical_end > kernel_physical_base);

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

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

	// Choose a canonical high-half base for the current VALEN.
	std::uintptr_t low_mask = 0;
	for (std::uint8_t i = 0; i < address_bits.virtual_address_bits; i++) {
		low_mask = (low_mask << 1) | 1u;
	}
	const std::uintptr_t upper_mask = ~low_mask;
	const std::uintptr_t sign_bit = (low_mask + 1) >> 1;
	const std::uintptr_t canonical_high_half_base = upper_mask | sign_bit;

	KernelVirtualAddressAllocator allocator;
	const std::uintptr_t managed_base = canonical_high_half_base + 0x01000000ull; // +16 MiB
	const std::uintptr_t managed_limit = managed_base + (4u * PhysicalMemoryManager::kPageSizeBytes);
	ROCINANTE_EXPECT_TRUE(ctx, managed_limit > managed_base);
	allocator.Init(managed_base, managed_limit);
	ROCINANTE_EXPECT_TRUE(ctx, allocator.IsInitialized());

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	const auto physical_page_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, physical_page_or.has_value());
	if (!physical_page_or.has_value()) return;
	const std::uintptr_t physical_base = physical_page_or.value();

	const auto mapped_or = MapPhysicalRange4KiB(
		&pmm,
		root_or.value(),
		&allocator,
		physical_base,
		PhysicalMemoryManager::kPageSizeBytes,
		permissions,
		address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, mapped_or.has_value());
	if (!mapped_or.has_value()) return;

	ROCINANTE_EXPECT_TRUE(ctx, mapped_or.value().virtual_base >= managed_base);
	ROCINANTE_EXPECT_TRUE(ctx, (mapped_or.value().virtual_base + mapped_or.value().size_bytes) <= managed_limit);

	const auto translated = Translate(root_or.value(), mapped_or.value().virtual_base, address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, translated.has_value());
	if (translated.has_value()) {
		ROCINANTE_EXPECT_EQ_U64(ctx, translated.value(), physical_base);
	}

	ROCINANTE_EXPECT_TRUE(ctx, UnmapAndFree4KiB(
		root_or.value(),
		&allocator,
		mapped_or.value().virtual_base,
		mapped_or.value().size_bytes,
		address_bits));

	// The freed VA range should be reusable.
	const auto second_physical_page_or = pmm.AllocatePage();
	ROCINANTE_EXPECT_TRUE(ctx, second_physical_page_or.has_value());
	if (!second_physical_page_or.has_value()) return;
	const std::uintptr_t second_physical_base = second_physical_page_or.value();

	const auto mapped_again_or = MapPhysicalRange4KiB(
		&pmm,
		root_or.value(),
		&allocator,
		second_physical_base,
		PhysicalMemoryManager::kPageSizeBytes,
		permissions,
		address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, mapped_again_or.has_value());
	if (!mapped_again_or.has_value()) return;

	ROCINANTE_EXPECT_EQ_U64(ctx, mapped_again_or.value().virtual_base, mapped_or.value().virtual_base);

	const auto translated_again = Translate(root_or.value(), mapped_again_or.value().virtual_base, address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, translated_again.has_value());
	if (translated_again.has_value()) {
		ROCINANTE_EXPECT_EQ_U64(ctx, translated_again.value(), second_physical_base);
	}

	ROCINANTE_EXPECT_TRUE(ctx, UnmapAndFree4KiB(
		root_or.value(),
		&allocator,
		mapped_again_or.value().virtual_base,
		mapped_again_or.value().size_bytes,
		address_bits));
}

static void Test_KernelMappings_MapNewRange4KiB(TestContext* ctx) {
	using Rocinante::Memory::BootMemoryRegion;
	using Rocinante::Memory::BootMemoryMap;
	using Rocinante::Memory::KernelVirtualAddressAllocator;
	using Rocinante::Memory::PhysicalMemoryManager;
	using Rocinante::Memory::KernelMappings::MapNewRange4KiB;
	using Rocinante::Memory::KernelMappings::UnmapAndFree4KiB;
	using Rocinante::Memory::Paging::AccessPermissions;
	using Rocinante::Memory::Paging::AddressSpaceBits;
	using Rocinante::Memory::Paging::AllocateRootPageTable;
	using Rocinante::Memory::Paging::CacheMode;
	using Rocinante::Memory::Paging::ExecutePermissions;
	using Rocinante::Memory::Paging::PagePermissions;
	using Rocinante::Memory::Paging::Translate;

	auto& cpucfg = Rocinante::GetCPUCFG();
	const AddressSpaceBits address_bits{
		.virtual_address_bits = static_cast<std::uint8_t>(cpucfg.VirtualAddressBits()),
		.physical_address_bits = static_cast<std::uint8_t>(cpucfg.PhysicalAddressBits()),
	};
	ROCINANTE_EXPECT_TRUE(ctx, address_bits.virtual_address_bits > 0);
	ROCINANTE_EXPECT_TRUE(ctx, address_bits.virtual_address_bits < 64);

	static constexpr std::uintptr_t kUsableBase = 0x01000000; // 16 MiB
	static constexpr std::size_t kUsableSizeBytes = 8u * 1024u * 1024u; // 8 MiB

	const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
	const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);
	ROCINANTE_EXPECT_TRUE(ctx, kernel_physical_end > kernel_physical_base);

	BootMemoryMap map;
	map.Clear();
	ROCINANTE_EXPECT_TRUE(ctx, map.AddRegion(BootMemoryRegion{.physical_base = kUsableBase, .size_bytes = kUsableSizeBytes, .type = BootMemoryRegion::Type::UsableRAM}));

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

	std::uintptr_t low_mask = 0;
	for (std::uint8_t i = 0; i < address_bits.virtual_address_bits; i++) {
		low_mask = (low_mask << 1) | 1u;
	}
	const std::uintptr_t upper_mask = ~low_mask;
	const std::uintptr_t sign_bit = (low_mask + 1) >> 1;
	const std::uintptr_t canonical_high_half_base = upper_mask | sign_bit;

	KernelVirtualAddressAllocator allocator;
	const std::uintptr_t managed_base = canonical_high_half_base + 0x02000000ull; // +32 MiB
	const std::uintptr_t managed_limit = managed_base + (8u * PhysicalMemoryManager::kPageSizeBytes);
	allocator.Init(managed_base, managed_limit);
	ROCINANTE_EXPECT_TRUE(ctx, allocator.IsInitialized());

	const PagePermissions permissions{
		.access = AccessPermissions::ReadWrite,
		.execute = ExecutePermissions::NoExecute,
		.cache = CacheMode::CoherentCached,
		.global = true,
	};

	static constexpr std::size_t kPageCount = 4;
	static constexpr std::size_t kSizeBytes = kPageCount * PhysicalMemoryManager::kPageSizeBytes;

	const auto mapped_or = MapNewRange4KiB(
		&pmm,
		root_or.value(),
		&allocator,
		kSizeBytes,
		permissions,
		address_bits);
	ROCINANTE_EXPECT_TRUE(ctx, mapped_or.has_value());
	if (!mapped_or.has_value()) return;
	ROCINANTE_EXPECT_EQ_U64(ctx, mapped_or.value().size_bytes, kSizeBytes);

	for (std::size_t i = 0; i < kPageCount; i++) {
		const std::uintptr_t page_virtual = mapped_or.value().virtual_base + (i * PhysicalMemoryManager::kPageSizeBytes);
		const auto translated = Translate(root_or.value(), page_virtual, address_bits);
		ROCINANTE_EXPECT_TRUE(ctx, translated.has_value());
		if (translated.has_value()) {
			ROCINANTE_EXPECT_EQ_U64(ctx, translated.value() % PhysicalMemoryManager::kPageSizeBytes, 0);
		}
	}

	ROCINANTE_EXPECT_TRUE(ctx, UnmapAndFree4KiB(
		root_or.value(),
		&allocator,
		mapped_or.value().virtual_base,
		mapped_or.value().size_bytes,
		address_bits));
}

} // namespace

void TestEntry_KernelMappings_MapTranslateUnmapAndFree(TestContext* ctx) {
	Test_KernelMappings_MapTranslateUnmapAndFree(ctx);
	Test_KernelMappings_MapNewRange4KiB(ctx);
}

} // namespace Rocinante::Testing

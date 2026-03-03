/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/boot/boot_print.h>

#include <src/memory/pmm.h>
#include <src/sp/uart16550.h>

namespace {

static const char* BootMemoryRegionTypeToString(Rocinante::Memory::BootMemoryRegion::Type type) {
	switch (type) {
		case Rocinante::Memory::BootMemoryRegion::Type::UsableRAM:
			return "UsableRAM";
		case Rocinante::Memory::BootMemoryRegion::Type::Reserved:
			return "Reserved";
	}
	return "Unknown";
}

} // namespace

namespace Rocinante::Boot {

void PrintBootMemoryMap(const Rocinante::Uart16550& uart, const Rocinante::Memory::BootMemoryMap& map) {
	uart.puts("Boot memory map (DTB):\n");
	uart.puts("  Region count: ");
	uart.write_dec_u64(map.region_count);
	uart.putc('\n');

	for (std::size_t i = 0; i < map.region_count; i++) {
		const auto& r = map.regions[i];
		uart.puts("  - ");
		uart.puts(BootMemoryRegionTypeToString(r.type));
		uart.puts(" base=");
		uart.write_dec_u64(r.physical_base);
		uart.puts(" size_bytes=");
		uart.write_dec_u64(r.size_bytes);
		uart.putc('\n');
	}
}

void PrintPhysicalMemoryManagerSummary(const Rocinante::Uart16550& uart, const Rocinante::Memory::PhysicalMemoryManager& pmm) {
	uart.puts("PMM summary:\n");
	uart.puts("  Tracked physical base:  ");
	uart.write_dec_u64(pmm.TrackedPhysicalBase());
	uart.putc('\n');
	uart.puts("  Tracked physical limit: ");
	uart.write_dec_u64(pmm.TrackedPhysicalLimit());
	uart.putc('\n');
	uart.puts("  Total pages: ");
	uart.write_dec_u64(pmm.TotalPages());
	uart.putc('\n');
	uart.puts("  Free pages:  ");
	uart.write_dec_u64(pmm.FreePages());
	uart.putc('\n');
}

} // namespace Rocinante::Boot

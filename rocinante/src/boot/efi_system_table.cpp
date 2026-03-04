/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/boot/efi_system_table.h>

#include <src/memory/boot_memory_map.h>

#include <cstddef>
#include <cstdint>

extern "C" void* memcpy(void* destination, const void* source, std::size_t byte_count);

namespace Rocinante::Boot {

bool EfiSystemTable::_copy_from_physical(void* dst, std::uintptr_t src_physical_address, std::size_t bytes) {
	if (!dst) return false;
	if (src_physical_address == 0) return false;
	if (bytes == 0) return true;

	// At kernel entry (before paging bring-up) Rocinante executes in direct
	// address translation mode, so low physical addresses are directly
	// accessible by pointer dereference.
	//
	// Flaw:
	// - This assumes the boot environment maps the EFI boot info region into an
	//   addressable range for the kernel at entry.
	// - If this is false on real hardware, we'll need a platform-specific early
	//   mapping strategy.
	const void* src = reinterpret_cast<const void*>(src_physical_address);
	memcpy(dst, src, bytes);
	return true;
}

Rocinante::Optional<EfiSystemTable> EfiSystemTable::TryCreateFromPhysicalAddress(std::uintptr_t efi_system_table_physical_address) {
	SystemTableRaw raw{};
	if (!_copy_from_physical(&raw, efi_system_table_physical_address, sizeof(raw))) {
		return Rocinante::nullopt;
	}

	if (raw.header.signature != kEfiSystemTableSignature) {
		return Rocinante::nullopt;
	}

	// Minimal sanity check: avoid reading absurd table counts.
	if (raw.number_of_table_entries > kMaxConfigurationTableEntries) {
		return Rocinante::nullopt;
	}

	if (raw.configuration_table_physical_address == 0) {
		return Rocinante::nullopt;
	}

	return EfiSystemTable(efi_system_table_physical_address, raw);
}

Rocinante::Optional<std::uintptr_t> EfiSystemTable::TryGetDeviceTreePhysicalAddress() const {
	if (m_configuration_table_physical_address == 0) return Rocinante::nullopt;
	if (m_configuration_table_entry_count == 0) return Rocinante::nullopt;

	for (std::uint64_t i = 0; i < m_configuration_table_entry_count; i++) {
		const std::uintptr_t entry_physical_address = m_configuration_table_physical_address + (i * sizeof(ConfigurationTableEntry));
		ConfigurationTableEntry entry{};
		if (!_copy_from_physical(&entry, entry_physical_address, sizeof(entry))) {
			return Rocinante::nullopt;
		}

		if (!entry.guid.Equals(kDeviceTreeGuid)) continue;

		const std::uintptr_t dtb_physical_address = static_cast<std::uintptr_t>(entry.table_physical_address);
		if (dtb_physical_address == 0) return Rocinante::nullopt;

		const void* dtb = reinterpret_cast<const void*>(dtb_physical_address);
		if (!Rocinante::Memory::BootMemoryMap::LooksLikeDeviceTreeBlob(dtb)) return Rocinante::nullopt;

		const std::size_t total_size_bytes = Rocinante::Memory::BootMemoryMap::DeviceTreeTotalSizeBytesOrZero(dtb);
		if (total_size_bytes == 0) return Rocinante::nullopt;

		return dtb_physical_address;
	}

	return Rocinante::nullopt;
}

} // namespace Rocinante::Boot

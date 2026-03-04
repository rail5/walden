/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>

namespace Rocinante::Boot {

/**
 * @brief A minimal UEFI EFI_SYSTEM_TABLE parser.
 *
 * Anchors:
 * - Linux LoongArch boot protocol (Linux-conventional):
 *   https://docs.kernel.org/arch/loongarch/booting.html
 *   defines entry registers: a0 = UEFI-bootenv flag, a1 = cmdline, a2 = EFI system table pointer.
 * - QEMU LoongArch direct-kernel boot (authoritative for QEMU `-machine virt`):
 *   QEMU builds an EFI system table in low memory and passes its (physical) address in a2.
 *   See upstream QEMU source: hw/loongarch/boot.c (init_systab/init_efi_fdt_table).
 *
 * Current policy:
 * - This class only implements what Rocinante needs right now: locating the
 *   Device Tree Blob (DTB / FDT) via EFI Configuration Tables.
 * - It does not attempt to validate CRCs, parse vendor strings, or implement
 *   Runtime/Boot services.
 */
class EfiSystemTable final {
	public:
		/**
		 * @brief UEFI GUID / EFI_GUID as stored in memory.
		 *
		 * UEFI GUIDs are 16 bytes. We store and compare them as raw bytes.
		 */
		struct Guid final {
			std::uint8_t bytes[16];

			constexpr bool Equals(const Guid& other) const {
				for (std::size_t i = 0; i < 16; i++) {
					if (bytes[i] != other.bytes[i]) return false;
				}
				return true;
			}
		};

		/**
		 * @brief The EFI_SYSTEM_TABLE signature defined by UEFI.
		 *
		 * Source anchor:
		 * - QEMU defines this as EFI_SYSTEM_TABLE_SIGNATURE in include/hw/loongarch/boot.h.
		 */
		static constexpr std::uint64_t kEfiSystemTableSignature = 0x5453595320494249ull;

		/**
		 * @brief EFI Configuration Table GUID for the Device Tree (DTB / FDT).
		 *
		 * Source anchor:
		 * - QEMU defines this GUID as DEVICE_TREE_GUID in include/hw/loongarch/boot.h.
		 * - This is the standard UEFI Device Tree GUID.
		 */
		static constexpr Guid kDeviceTreeGuid = Guid{{
			0xd5, 0x21, 0xb6, 0xb1, // 0xb1b621d5 (little-endian bytes)
			0x9c, 0xf1,             // 0xf19c
			0xa5, 0x41,             // 0x41a5
			0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0
		}};

		/**
		 * @brief Try to parse an EFI system table at a physical address.
		 *
		 * @param efi_system_table_physical_address Physical address passed in register a2.
		 */
		static Rocinante::Optional<EfiSystemTable> TryCreateFromPhysicalAddress(std::uintptr_t efi_system_table_physical_address);

		/**
		 * @brief Return the physical address of the EFI system table (a2).
		 */
		std::uintptr_t PhysicalAddress() const { return m_physical_address; }

		/**
		 * @brief Number of EFI configuration table entries advertised by the system table.
		 */
		std::uint64_t ConfigurationTableEntryCount() const { return m_configuration_table_entry_count; }

		/**
		 * @brief Try to locate the DTB physical address via the Device Tree GUID.
		 */
		Rocinante::Optional<std::uintptr_t> TryGetDeviceTreePhysicalAddress() const;

	private:
		static constexpr std::uint64_t kMaxConfigurationTableEntries = 64;

		// UEFI structs needed for parsing. We treat all pointer-typed fields as
		// physical addresses at boot time.
		struct TableHeader final {
			std::uint64_t signature;
			std::uint32_t revision;
			std::uint32_t header_size;
			std::uint32_t crc32;
			std::uint32_t reserved;
		};

		struct ConfigurationTableEntry final {
			Guid guid;
			std::uint64_t table_physical_address;
		};

		struct SystemTableRaw final {
			TableHeader header;
			std::uint64_t firmware_vendor_physical_address;
			std::uint32_t firmware_revision;
			std::uint32_t padding;
			std::uint64_t console_in_handle;
			std::uint64_t console_in;
			std::uint64_t console_out_handle;
			std::uint64_t console_out;
			std::uint64_t standard_error_handle;
			std::uint64_t standard_error;
			std::uint64_t runtime_services;
			std::uint64_t boot_services;
			std::uint64_t number_of_table_entries;
			std::uint64_t configuration_table_physical_address;
		};

		static_assert(sizeof(TableHeader) == 24);
		static_assert(sizeof(ConfigurationTableEntry) == 24);
		static_assert(sizeof(SystemTableRaw) == 120);

		explicit EfiSystemTable(std::uintptr_t physical_address, const SystemTableRaw& raw)
			: m_physical_address(physical_address)
			, m_configuration_table_entry_count(raw.number_of_table_entries)
			, m_configuration_table_physical_address(static_cast<std::uintptr_t>(raw.configuration_table_physical_address))
		{}

		static bool _copy_from_physical(void* dst, std::uintptr_t src_physical_address, std::size_t bytes);

		std::uintptr_t m_physical_address;
		std::uint64_t m_configuration_table_entry_count;
		std::uintptr_t m_configuration_table_physical_address;
};

} // namespace Rocinante::Boot

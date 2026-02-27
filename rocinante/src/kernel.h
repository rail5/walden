/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

extern "C" {
	/**
	 * @brief The main entry point of the kernel, called by the assembly language bootstrapping code after basic CPU setup is complete.
	 *
	 * @param is_uefi_compliant_bootenv Nonzero if the boot environment is UEFI-compliant, zero otherwise. Contents of the a0 register.
	 * @param kernel_cmdline_ptr Pointer to a null-terminated ASCII string containing the kernel command line, or null if no command line was provided. Contents of the a1 register.
	 * @param efi_system_table_ptr Pointer to the EFI system table. Contents of the a2 register.
	 * 
	 */
	[[noreturn]] void kernel_main(
		std::uint64_t is_uefi_compliant_bootenv /* a0 */,
		std::uint64_t kernel_cmdline_ptr /* a1 */,
		std::uint64_t efi_system_table_ptr /* a2 */
	);
}

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "kernel.h"

#include <src/memory/memory.h>
#include <src/sp/cpucfg.h>
#include <src/sp/uart16550.h>
#include <src/helpers/optional.h>

namespace {

constexpr uintptr_t UART_BASE = 0x1fe001e0UL; // QEMU LoongArch virt: VIRT_UART_BASE address

static constinit Rocinante::Uart16550 uart(UART_BASE);

[[noreturn]] static inline void halt() {
	for (;;) {
		asm volatile("idle 0" ::: "memory");
	}
}

} // namespace

extern "C" [[noreturn]] void kernel_main(uint64_t is_uefi_compliant_bootenv, uint64_t kernel_cmdline_ptr, uint64_t efi_system_table_ptr) {
	Rocinante::Memory::InitEarly();

	auto& cpucfg = Rocinante::GetCPUCFG();

	Rocinante::String info;
	info += "Hello, Rocinante!\n";
	info += "Don the LoongArch64 armor and prepare to ride!\n\n";

	uart.write(info);

	// Print some information about the CPU configuration using the CPUCFG class
	uart.puts("CPU Architecture: ");
	switch (cpucfg.Arch()) {
		case Rocinante::CPUCFG::Architecture::SimplifiedLA32:
			uart.puts("Simplified LA32\n");
			break;
		case Rocinante::CPUCFG::Architecture::LA32:
			uart.puts("LA32\n");
			break;
		case Rocinante::CPUCFG::Architecture::LA64:
			uart.puts("LA64\n");
			break;
		case Rocinante::CPUCFG::Architecture::Reserved:
			uart.puts("Reserved/Unknown\n");
			break;
	}

	uart.putc('\n');

	if (cpucfg.MMUSupportsPageMappingMode()) {
		uart.puts("MMU supports page mapping mode\n");
	} else {
		uart.puts("MMU does not support page mapping mode\n");
	}

	uart.putc('\n');

	// Let's read and print VALEN/PALEN as a sanity check that our CPUCFG class is working and we can read CPU-reported information correctly.
	uart.puts("Supported virtual address bits (VALEN): ");
	uart.write(Rocinante::to_string(cpucfg.VirtualAddressBits()));
	uart.putc('\n');
	uart.puts("Supported physical address bits (PALEN): ");
	uart.write(Rocinante::to_string(cpucfg.PhysicalAddressBits()));
	uart.putc('\n');

	uart.putc('\n');

	// &c...

	uart.puts("Contents of the a0 register (is_uefi_compliant_bootenv): ");
	uart.write(Rocinante::to_string(is_uefi_compliant_bootenv));
	uart.putc('\n');

	uart.puts("Contents of the a1 register (kernel_cmdline_ptr): ");
	if (kernel_cmdline_ptr) {
		uart.puts(reinterpret_cast<const char*>(kernel_cmdline_ptr));
	} else {
		uart.puts("(null)");
	}
	uart.putc('\n');

	uart.puts("Contents of the a2 register (efi_system_table_ptr): ");
	if (efi_system_table_ptr) {
		uart.write(Rocinante::to_string(efi_system_table_ptr));
	} else {
		uart.puts("(null)");
	}
	uart.putc('\n');

	halt();
}

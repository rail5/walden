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

extern "C" [[noreturn]] void kernel_main(uint64_t a0, uint64_t a1, uint64_t a2) {
	Rocinante::Memory::InitEarly();

	auto& cpucfg = Rocinante::GetCPUCFG();

	Rocinante::String info;
	info += "Hello, Rocinante!\n";
	info += "Don the LoongArch64 armor and prepare to ride!\n";

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

	if (cpucfg.MMUSupportsPageMappingMode()) {
		uart.puts("MMU supports page mapping mode\n");
	} else {
		uart.puts("MMU does not support page mapping mode\n");
	}

	// Let's read and print VALEN/PALEN as a sanity check that our CPUCFG class is working and we can read CPU-reported information correctly.
	uart.puts("Supported virtual address bits (VALEN): ");
	uart.write(Rocinante::to_string(cpucfg.VirtualAddressBits()));
	uart.puts("\n");
	uart.puts("Supported physical address bits (PALEN): ");
	uart.write(Rocinante::to_string(cpucfg.PhysicalAddressBits()));
	uart.puts("\n");

	// &c...

	halt();
}

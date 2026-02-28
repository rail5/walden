/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "kernel.h"

#include <src/memory/memory.h>
#include <src/memory/boot_memory_map.h>
#include <src/sp/cpucfg.h>
#include <src/sp/uart16550.h>
#include <src/sp/mmio.h>
#include <src/helpers/optional.h>
#include <src/trap.h>

#include <src/testing/test.h>

namespace {

constexpr std::uintptr_t UART_BASE = 0x1fe001e0UL; // QEMU LoongArch virt: VIRT_UART_BASE address

static constinit Rocinante::Uart16550 uart(UART_BASE);

static const char* BootMemoryRegionTypeToString(Rocinante::Memory::BootMemoryRegion::Type type) {
	switch (type) {
		case Rocinante::Memory::BootMemoryRegion::Type::UsableRAM:
			return "UsableRAM";
		case Rocinante::Memory::BootMemoryRegion::Type::Reserved:
			return "Reserved";
	}
	return "Unknown";
}

static void PrintBootMemoryMap(const Rocinante::Uart16550& uart, const Rocinante::Memory::BootMemoryMap& map) {
	uart.puts("Boot memory map (DTB):\n");
	uart.puts("  Region count: ");
	uart.write(Rocinante::to_string(map.region_count));
	uart.putc('\n');

	for (std::size_t i = 0; i < map.region_count; i++) {
		const auto& r = map.regions[i];
		uart.puts("  - ");
		uart.puts(BootMemoryRegionTypeToString(r.type));
		uart.puts(" base=");
		uart.write(Rocinante::to_string(r.physical_base));
		uart.puts(" size_bytes=");
		uart.write(Rocinante::to_string(r.size_bytes));
		uart.putc('\n');
	}
}

[[noreturn]] static inline void halt() {
	for (;;) {
		asm volatile("idle 0" ::: "memory");
	}
}

[[noreturn]] [[maybe_unused]] static inline void shutdown() {
	// QEMU LoongArch64 virt poweroff is wired up as a "syscon-poweroff" device.
	// The virt machine advertises this via its DTB:
	// - /poweroff compatible = "syscon-poweroff"
	// - regmap -> syscon at 0x100e001c (reg-io-width = 1)
	// - offset = 0, value = 0x34
	// Writing that byte triggers a QEMU shutdown event, and QEMU exits by default
	// (-action shutdown=poweroff).
	constexpr std::uintptr_t kSysconBase = 0x100e001cUL;
	constexpr std::uintptr_t kPoweroffOffset = 0;
	constexpr std::uint8_t kPoweroffValue = 0x34;

	Rocinante::MMIO<8>::write(kSysconBase + kPoweroffOffset, kPoweroffValue);
	asm volatile("dbar 0" ::: "memory");

	// If QEMU ignores the poweroff request, just stop.
	halt();
}


} // namespace

extern "C" void RocinanteTrapHandler(Rocinante::TrapFrame* tf) {
	const std::uint64_t exception_code =
		Rocinante::Trap::ExceptionCodeFromExceptionStatus(tf->exception_status);
	const std::uint64_t exception_subcode =
		Rocinante::Trap::ExceptionSubCodeFromExceptionStatus(tf->exception_status);
	const std::uint64_t interrupt_status =
		Rocinante::Trap::InterruptStatusFromExceptionStatus(tf->exception_status);

	#if defined(ROCINANTE_TESTS)
	if (Rocinante::Testing::HandleTrap(tf, exception_code, exception_subcode, interrupt_status)) {
		return;
	}
	#else

	// LoongArch EXCCODE values (subset used for early bring-up).
	constexpr std::uint64_t kExceptionCodeBreak = 0x0c;
	constexpr std::uint64_t kTimerInterruptLineBit = (1ull << 11);

	// Interrupts arrive with EXCCODE=0 and the pending lines in ESTAT.IS.
	if (exception_code == 0 && (interrupt_status & kTimerInterruptLineBit) != 0) {
		Rocinante::Trap::ClearTimerInterrupt();
		Rocinante::Trap::StopTimer();
		return;
	}

	if (exception_code == kExceptionCodeBreak) {
		uart.puts("\n*** TRAP: BRK ***\n");
		uart.puts("ERA:   ");
		uart.write(Rocinante::to_string(tf->exception_return_address));
		uart.putc('\n');
		uart.puts("ESTAT: ");
		uart.write(Rocinante::to_string(tf->exception_status));
		uart.putc('\n');
		uart.puts("SUB:   ");
		uart.write(Rocinante::to_string(exception_subcode));
		uart.putc('\n');

		// Skip the BREAK instruction so we can prove ERTN return works.
		// LoongArch instructions are 32-bit.
		tf->exception_return_address += 4;
		return;
	}
	#endif

	uart.puts("\n*** TRAP ***\n");
	uart.puts("ERA:   ");
	uart.write(Rocinante::to_string(tf->exception_return_address));
	uart.putc('\n');
	uart.puts("ESTAT: ");
	uart.write(Rocinante::to_string(tf->exception_status));
	uart.putc('\n');
	uart.puts("BADV:  ");
	uart.write(Rocinante::to_string(tf->bad_virtual_address));
	uart.putc('\n');
	uart.puts("EXC:   ");
	uart.write(Rocinante::to_string(exception_code));
	uart.puts(" SUB: ");
	uart.write(Rocinante::to_string(exception_subcode));
	uart.putc('\n');

	halt();
}

extern "C" [[noreturn]] void kernel_main(std::uint64_t is_uefi_compliant_bootenv, std::uint64_t kernel_cmdline_ptr, std::uint64_t efi_system_table_ptr) {
	Rocinante::Memory::InitEarly();
	Rocinante::Trap::Initialize();

	// Read the kernel command line from the pointer passed in a1 by the boot environment, if present.
	if (kernel_cmdline_ptr) {
		const char* cmdline = reinterpret_cast<const char*>(kernel_cmdline_ptr);
		uart.puts("Kernel command line: ");
		uart.puts(cmdline);
		uart.putc('\n');
	}

	#if defined(ROCINANTE_TESTS)
	const int failed = Rocinante::Testing::RunAll(&uart);
	if (failed == 0) {
		uart.puts("\nALL TESTS PASSED\n");
	} else {
		uart.puts("\nTESTS FAILED\n");
	}
	shutdown();
	#endif

	// QEMU's direct-kernel boot convention is platform/firmware-defined; for the
	// LoongArch virt machine it commonly passes a DTB pointer in a2.
	//
	// We currently name this parameter `efi_system_table_ptr` because we expect
	// to support a UEFI-compliant boot environment later. For bring-up, we detect
	// a DTB structurally and parse it if present.
	const void* maybe_device_tree_blob = reinterpret_cast<const void*>(efi_system_table_ptr);
	if (Rocinante::Memory::BootMemoryMap::LooksLikeDeviceTreeBlob(maybe_device_tree_blob)) {
		Rocinante::Memory::BootMemoryMap boot_map;
		if (boot_map.TryParseFromDeviceTree(maybe_device_tree_blob)) {
			PrintBootMemoryMap(uart, boot_map);
		} else {
			uart.puts("DTB detected but failed to parse boot memory map\n");
		}
	} else {
		uart.puts("No DTB detected in a2; skipping boot memory map parse\n");
	}

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

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "kernel.h"

#include <src/memory/memory.h>
#include <src/sp/cpucfg.h>
#include <src/sp/uart16550.h>
#include <src/helpers/optional.h>
#include <src/trap.h>

namespace {

constexpr uintptr_t UART_BASE = 0x1fe001e0UL; // QEMU LoongArch virt: VIRT_UART_BASE address

static constinit Rocinante::Uart16550 uart(UART_BASE);

#if defined(ROCINANTE_SELFTEST_TIMER_IRQ)
static volatile std::uint64_t g_timer_irq_seen = 0;

static inline std::uint64_t rdtime_d() {
	std::uint64_t value;
	asm volatile("rdtime.d %0, $zero" : "=r"(value));
	return value;
}
#endif

[[noreturn]] static inline void halt() {
	for (;;) {
		asm volatile("idle 0" ::: "memory");
	}
}

} // namespace

extern "C" void RocinanteTrapHandler(Rocinante::TrapFrame* tf) {
	const std::uint64_t exception_code =
		Rocinante::Trap::ExceptionCodeFromExceptionStatus(tf->exception_status);
	const std::uint64_t exception_subcode =
		Rocinante::Trap::ExceptionSubCodeFromExceptionStatus(tf->exception_status);
	const std::uint64_t interrupt_status =
		Rocinante::Trap::InterruptStatusFromExceptionStatus(tf->exception_status);

	// LoongArch EXCCODE values (subset used for early bring-up).
	constexpr std::uint64_t kExceptionCodeBreak = 0x0c;
	constexpr std::uint64_t kTimerInterruptLineBit = (1ull << 11);

	// Interrupts arrive with EXCCODE=0 and the pending lines in ESTAT.IS.
	if (exception_code == 0 && (interrupt_status & kTimerInterruptLineBit) != 0) {
		Rocinante::Trap::ClearTimerInterrupt();
		Rocinante::Trap::StopTimer();
		#if defined(ROCINANTE_SELFTEST_TIMER_IRQ)
		g_timer_irq_seen = 1;
		#endif
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

extern "C" [[noreturn]] void kernel_main(uint64_t is_uefi_compliant_bootenv, uint64_t kernel_cmdline_ptr, uint64_t efi_system_table_ptr) {
	Rocinante::Memory::InitEarly();
	Rocinante::Trap::Initialize();

	auto& cpucfg = Rocinante::GetCPUCFG();

	Rocinante::String info;
	info += "Hello, Rocinante!\n";
	info += "Don the LoongArch64 armor and prepare to ride!\n\n";

	uart.write(info);

	#if defined(ROCINANTE_SELFTEST_TRAPS)
	uart.puts("Triggering BREAK self-test...\n");
	asm volatile("break 0" ::: "memory");
	uart.puts("BREAK returned OK (ERTN path works)\n\n");
	#endif

	#if defined(ROCINANTE_SELFTEST_TIMER_IRQ)
	uart.puts("Triggering timer IRQ self-test...\n");
	Rocinante::Trap::DisableInterrupts();
	Rocinante::Trap::MaskAllInterruptLines();
	Rocinante::Trap::StartOneShotTimerTicks(100000); // small delay
	Rocinante::Trap::UnmaskTimerInterruptLine();
	Rocinante::Trap::EnableInterrupts();

	const std::uint64_t t0 = rdtime_d();
	while (g_timer_irq_seen == 0) {
		const std::uint64_t now = rdtime_d();
		if ((now - t0) > 50000000ull) { // ~0.5s at 100MHz (approx; good enough for bring-up)
			break;
		}
		asm volatile("nop" ::: "memory");
	}

	if (g_timer_irq_seen != 0) {
		uart.puts("Timer IRQ received (interrupt delivery works)\n\n");
	} else {
		uart.puts("Timer IRQ timed out (interrupt delivery NOT working yet)\n\n");
	}
	Rocinante::Trap::DisableInterrupts();
	#endif

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

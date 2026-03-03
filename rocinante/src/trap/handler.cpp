/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/trap/trap.h>

#include <src/platform/console.h>
#include <src/platform/power.h>

#include <src/sp/uart16550.h>

#include <src/testing/test.h>

#include <cstdint>

namespace {

namespace Csr {
	// LoongArch privileged architecture CSR numbering.
	constexpr std::uint32_t kTlbIndex = 0x10;   // CSR.TLBIDX
	constexpr std::uint32_t kTlbEntryHigh = 0x11; // CSR.TLBEHI
	constexpr std::uint32_t kAddressSpaceId = 0x18; // CSR.ASID
	constexpr std::uint32_t kPgdLow = 0x19;     // CSR.PGDL
	constexpr std::uint32_t kPgdHigh = 0x1A;    // CSR.PGDH
	constexpr std::uint32_t kPgd = 0x1B;        // CSR.PGD (read-only)
	constexpr std::uint32_t kPageWalkControlLow = 0x1C;  // CSR.PWCL
	constexpr std::uint32_t kPageWalkControlHigh = 0x1D; // CSR.PWCH
	constexpr std::uint32_t kReducedVirtualAddressConfiguration = 0x1F; // CSR.RVACFG

	constexpr std::uint32_t kTlbRefillEntryAddress = 0x88; // CSR.TLBRENTRY
	constexpr std::uint32_t kTlbRefillBadVirtualAddress = 0x89; // CSR.TLBRBADV
	constexpr std::uint32_t kTlbRefillExceptionReturnAddress = 0x8A; // CSR.TLBRERA
	constexpr std::uint32_t kTlbRefillEntryHigh = 0x8E; // CSR.TLBREHI
} // namespace Csr

template<std::uint32_t CsrNumber>
static inline std::uint64_t ReadCsr() {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(CsrNumber));
	return value;
}

static void DumpMappedTranslationCsrs(const Rocinante::Uart16550& uart) {
	const std::uint64_t pgdl = ReadCsr<Csr::kPgdLow>();
	const std::uint64_t pgdh = ReadCsr<Csr::kPgdHigh>();
	const std::uint64_t pgd = ReadCsr<Csr::kPgd>();
	const std::uint64_t pwcl = ReadCsr<Csr::kPageWalkControlLow>();
	const std::uint64_t pwch = ReadCsr<Csr::kPageWalkControlHigh>();
	const std::uint64_t rvacfg = ReadCsr<Csr::kReducedVirtualAddressConfiguration>();

	uart.puts("PGDL:  "); uart.write_hex_u64(pgdl); uart.putc('\n');
	uart.puts("PGDH:  "); uart.write_hex_u64(pgdh); uart.putc('\n');
	uart.puts("PGD:   "); uart.write_hex_u64(pgd); uart.putc('\n');
	uart.puts("PWCL:  "); uart.write_hex_u64(pwcl); uart.putc('\n');
	uart.puts("PWCH:  "); uart.write_hex_u64(pwch); uart.putc('\n');
	uart.puts("RVACFG:"); uart.write_hex_u64(rvacfg); uart.putc('\n');
}

static void DumpTlbRefillCsrsIfActive(const Rocinante::Uart16550& uart) {
	const std::uint64_t tlbrera = ReadCsr<Csr::kTlbRefillExceptionReturnAddress>();
	const bool is_tlbr = (tlbrera & 1ull) != 0;
	if (!is_tlbr) return;

	const std::uint64_t tlbrbadv = ReadCsr<Csr::kTlbRefillBadVirtualAddress>();
	const std::uint64_t tlbrehi = ReadCsr<Csr::kTlbRefillEntryHigh>();
	const std::uint64_t tlbrentry = ReadCsr<Csr::kTlbRefillEntryAddress>();

	uart.puts("TLBR:  IsTLBR=1\n");
	uart.puts("TLBRENTRY:"); uart.write_hex_u64(tlbrentry); uart.putc('\n');
	uart.puts("TLBRERA:  "); uart.write_hex_u64(tlbrera); uart.putc('\n');
	uart.puts("TLBRBADV: "); uart.write_hex_u64(tlbrbadv); uart.putc('\n');
	uart.puts("TLBREHI:  "); uart.write_hex_u64(tlbrehi); uart.putc('\n');
}

} // namespace

extern "C" void RocinanteTrapHandler(Rocinante::TrapFrame* tf) {
	auto& uart = Rocinante::Platform::GetEarlyUart();

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
	// ESTAT.IS bit 11 corresponds to the timer interrupt line (see src/trap.cpp).
	constexpr std::uint64_t kTimerInterruptLineBit = (1ull << 11);

	// Interrupts arrive with EXCCODE=0 and the pending lines in ESTAT.IS.
	if (exception_code == 0 && (interrupt_status & kTimerInterruptLineBit) != 0) {
		Rocinante::Trap::ClearTimerInterrupt();
		Rocinante::Trap::StopTimer();
		return;
	}

	if (exception_code == kExceptionCodeBreak) {
		uart.puts("\n*** TRAP: BRK ***\n");
		uart.puts("CSR.ERA (exception return address): ");
		uart.write_hex_u64(tf->exception_return_address);
		uart.putc('\n');
		uart.puts("CSR.ESTAT (exception status):       ");
		uart.write_hex_u64(tf->exception_status);
		uart.putc('\n');
		uart.puts("SUB:   "); uart.write_dec_u64(exception_subcode); uart.putc('\n');

		// Skip the BREAK instruction so we can prove ERTN return works.
		// LoongArch instructions are 32-bit.
		tf->exception_return_address += 4;
		return;
	}
	#endif

	uart.puts("\n*** TRAP ***\n");
	uart.puts("EXC="); uart.write_hex_u64(exception_code);
	uart.puts(" SUB="); uart.write_hex_u64(exception_subcode);
	uart.puts("\n");

	uart.puts("CSR.ERA (exception return address): ");
	uart.write_hex_u64(tf->exception_return_address);
	uart.putc('\n');
	uart.puts("CSR.ESTAT (exception status):       ");
	uart.write_hex_u64(tf->exception_status);
	uart.putc('\n');
	uart.puts("CSR.BADV (bad virtual address):     ");
	uart.write_hex_u64(tf->bad_virtual_address);
	uart.putc('\n');
	uart.puts("CSR.CRMD (current mode info):       ");
	uart.write_hex_u64(tf->current_mode_information);
	uart.putc('\n');
	uart.puts("CSR.PRMD (previous mode info):      ");
	uart.write_hex_u64(tf->previous_mode_information);
	uart.putc('\n');
	uart.puts("CSR.ECFG (exception config):        ");
	uart.write_hex_u64(tf->exception_configuration);
	uart.putc('\n');

	DumpTlbRefillCsrsIfActive(uart);
	DumpMappedTranslationCsrs(uart);

	// A few extra CSRs that are useful when debugging translation state.
	uart.puts("ASID:  "); uart.write_hex_u64(ReadCsr<Csr::kAddressSpaceId>()); uart.putc('\n');
	uart.puts("TLBIDX:"); uart.write_hex_u64(ReadCsr<Csr::kTlbIndex>()); uart.putc('\n');
	uart.puts("TLBEHI:"); uart.write_hex_u64(ReadCsr<Csr::kTlbEntryHigh>()); uart.putc('\n');

	Rocinante::Platform::Halt();
}

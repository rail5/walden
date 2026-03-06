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

static const char* ExceptionCodeName(std::uint64_t exception_code, bool is_tlbr) {
	// LoongArch Vol.1: Table 21 (Table of exception encoding).
	if (is_tlbr) return "TLBR";

	switch (exception_code) {
		case 0x1: return "PIL";
		case 0x2: return "PIS";
		case 0x3: return "PIF";
		case 0x4: return "PME";
		case 0x5: return "PNR";
		case 0x6: return "PNX";
		case 0x7: return "PPI";
		default: return nullptr;
	}
}

static bool IsPagingException(std::uint64_t exception_code) {
	// LoongArch Vol.1: Table 21 (Table of exception encoding).
	// Page-fault-related ecodes occupy the contiguous range [0x1, 0x7].
	return exception_code >= 0x1 && exception_code <= 0x7;
}

static const char* PagingAccessTypeNameOrNull(std::uint64_t exception_code) {
	// LoongArch Vol.1: Table 21 (Table of exception encoding).
	// - 0x1 PIL: page invalid for load
	// - 0x2 PIS: page invalid for store
	// - 0x3 PIF: page invalid for fetch
	// - 0x6 PNX: page non-executable exception
	switch (exception_code) {
		case 0x1: return "load";
		case 0x2: return "store";
		case 0x3: return "fetch";
		case 0x6: return "fetch";
		default: return nullptr;
	}
}

static Rocinante::Trap::PagingAccessType PagingAccessTypeFromExceptionCode(std::uint64_t exception_code) {
	// LoongArch Vol.1: Table 21 (Table of exception encoding).
	// - 0x1 PIL: page invalid for load
	// - 0x2 PIS: page invalid for store
	// - 0x3 PIF: page invalid for fetch
	// - 0x6 PNX: page non-executable exception
	switch (exception_code) {
		case 0x1: return Rocinante::Trap::PagingAccessType::Load;
		case 0x2: return Rocinante::Trap::PagingAccessType::Store;
		case 0x3: return Rocinante::Trap::PagingAccessType::Fetch;
		case 0x6: return Rocinante::Trap::PagingAccessType::Fetch;
		default: return Rocinante::Trap::PagingAccessType::Unknown;
	}
}

static std::uint64_t CurrentPrivilegeLevelFromCrmd(std::uint64_t crmd) {
	// LoongArch Vol.1: Section 7.4.1 (CRMD), Table 15.
	// CSR.CRMD.PLV is bits [1:0], legal values 0..3.
	static constexpr std::uint64_t kCrmdPrivilegeLevelMask = 0x3;
	return crmd & kCrmdPrivilegeLevelMask;
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

	// Page-fault dispatch hook (mechanism), independent of policy.
	//
	// Note: In ROCINANTE_TESTS builds, the test harness can early-return from the
	// trap handler. Dispatch must run before that so tests can validate the hook.
	const bool is_tlbr = (ReadCsr<Csr::kTlbRefillExceptionReturnAddress>() & 1ull) != 0;
	const bool is_paging_exception = (!is_tlbr) && IsPagingException(exception_code);
	if (is_paging_exception) {
		const std::uint64_t current_plv = CurrentPrivilegeLevelFromCrmd(tf->current_mode_information);

		// Address-space and root-page-table identity.
		//
		// Spec anchor (LoongArch-Vol1-EN.html):
		// - Vol.1 Section 7.5.4 (ASID), Table 38:
		//   - CSR.ASID.ASID is bits [9:0]
		//   - CSR.ASID.ASIDBITS is bits [23:16]
		// - Vol.1 Section 7.5.7 (PGD), Table 41:
		//   - CSR.PGD returns the effective root page directory base for the current BADV context.
		static constexpr std::uint64_t kAsidMask = 0x3ff;
		static constexpr std::uint64_t kAsidBitsShift = 16;
		static constexpr std::uint64_t kAsidBitsMask = 0xff;
		static constexpr std::uint64_t kPgdBaseMask = 0xfffffffffffff000ull;

		const std::uint64_t asid_csr = ReadCsr<Csr::kAddressSpaceId>();
		const std::uint64_t pgdl_csr = ReadCsr<Csr::kPgdLow>();
		const std::uint64_t pgdh_csr = ReadCsr<Csr::kPgdHigh>();
		const std::uint64_t pgd_csr = ReadCsr<Csr::kPgd>();

		const std::uint64_t pgdl_base = pgdl_csr & kPgdBaseMask;
		const std::uint64_t pgdh_base = pgdh_csr & kPgdBaseMask;
		const std::uint64_t pgd_base = pgd_csr & kPgdBaseMask;

		auto pgd_selection = Rocinante::Trap::PagingPgdSelection::Unknown;
		if (pgd_base == pgdl_base && pgd_base != pgdh_base) {
			pgd_selection = Rocinante::Trap::PagingPgdSelection::LowHalf;
		} else if (pgd_base == pgdh_base && pgd_base != pgdl_base) {
			pgd_selection = Rocinante::Trap::PagingPgdSelection::HighHalf;
		} else if (pgd_base == pgdl_base && pgd_base == pgdh_base) {
			// Early bring-up may set both halves to the same root.
			pgd_selection = Rocinante::Trap::PagingPgdSelection::LowHalf;
		}

		const Rocinante::Trap::PagingFaultEvent event{
			.exception_code = exception_code,
			.exception_subcode = exception_subcode,
			.exception_return_address = tf->exception_return_address,
			.bad_virtual_address = tf->bad_virtual_address,
			.current_mode_information = tf->current_mode_information,
			.previous_mode_information = tf->previous_mode_information,
			.current_privilege_level = current_plv,
			.address_space_id = static_cast<std::uint16_t>(asid_csr & kAsidMask),
			.address_space_id_bits = static_cast<std::uint8_t>((asid_csr >> kAsidBitsShift) & kAsidBitsMask),
			.pgd_selection = pgd_selection,
			.pgd_base = pgd_base,
			.pgdl_base = pgdl_base,
			.pgdh_base = pgdh_base,
			.access_type = PagingAccessTypeFromExceptionCode(exception_code),
		};

		if (Rocinante::Trap::DispatchPagingFault(*tf, event) == Rocinante::Trap::PagingFaultResult::Handled) {
			return;
		}
	}

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

	const char* exception_code_name = ExceptionCodeName(exception_code, is_tlbr);
	if (is_paging_exception) {
		uart.puts("\n*** PAGE FAULT ***\n");
	} else {
		uart.puts("\n*** TRAP ***\n");
	}

	uart.puts("EXC="); uart.write_hex_u64(exception_code);
	if (exception_code_name != nullptr) {
		uart.puts(" (");
		uart.puts(exception_code_name);
		uart.puts(")");
	}
	uart.puts(" SUB="); uart.write_hex_u64(exception_subcode);
	uart.puts("\n");

	if (is_paging_exception) {
		const std::uint64_t current_plv = CurrentPrivilegeLevelFromCrmd(tf->current_mode_information);
		uart.puts("CRMD.PLV (current privilege level): ");
		uart.write_dec_u64(current_plv);
		uart.putc('\n');

		const char* access_type = PagingAccessTypeNameOrNull(exception_code);
		if (access_type != nullptr) {
			uart.puts("Paging access type: ");
			uart.puts(access_type);
			uart.putc('\n');
		}

		uart.puts("Fault policy: HALT (no recovery implemented)\n");
	}

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

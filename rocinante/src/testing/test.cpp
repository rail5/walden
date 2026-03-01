/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <cstddef>
#include <src/sp/uart16550.h>
#include <src/trap.h>

namespace Rocinante::Testing {

namespace {

static volatile std::uint32_t g_break_trap_count = 0;
static volatile bool g_timer_interrupt_observed = false;

static volatile bool g_expected_trap_armed = false;
static volatile bool g_expected_trap_observed = false;
static volatile std::uint64_t g_expected_exception_code = 0;
static volatile std::uint64_t g_expected_exception_subcode = 0;

static volatile std::uint64_t g_observed_exception_code = 0;
static volatile std::uint64_t g_observed_exception_subcode = 0;
static volatile std::uint64_t g_observed_era = 0;
static volatile std::uint64_t g_observed_badv = 0;

// LoongArch instructions are fixed-width 32-bit.
static constexpr std::uint64_t kInstructionSizeBytes = 4;

// LoongArch exception codes (EXCCODE field in CSR.ESTAT).
static constexpr std::uint64_t kExceptionCodeInterrupt = 0x0;
static constexpr std::uint64_t kExceptionCodeBreak = 0x0c;

// LoongArch interrupt pending bits (CSR.ESTAT.IS).
// Bit 11 is the CPU-local timer interrupt line.
static constexpr std::uint64_t kTimerInterruptPendingBit = (1ull << 11);

static void Print(TestContext* ctx, const char* s) {
	ctx->uart->puts(s);
}

static void PrintU64(TestContext* ctx, std::uint64_t v) {
	ctx->uart->write_dec_u64(v);
}

} // namespace

void ResetTrapObservations() {
	g_break_trap_count = 0;
	g_timer_interrupt_observed = false;
	g_expected_trap_armed = false;
	g_expected_trap_observed = false;
	g_expected_exception_code = 0;
	g_expected_exception_subcode = 0;
	g_observed_exception_code = 0;
	g_observed_exception_subcode = 0;
	g_observed_era = 0;
	g_observed_badv = 0;
}

std::uint32_t BreakTrapCount() {
	return g_break_trap_count;
}

bool TimerInterruptObserved() {
	return g_timer_interrupt_observed;
}

void ArmExpectedTrap(std::uint64_t exception_code, std::uint64_t exception_subcode) {
	g_expected_exception_code = exception_code;
	g_expected_exception_subcode = exception_subcode;
	g_expected_trap_observed = false;
	g_expected_trap_armed = true;
}

bool ExpectedTrapObserved() {
	return g_expected_trap_observed;
}

std::uint64_t ExpectedTrapExceptionCode() {
	return g_observed_exception_code;
}

std::uint64_t ExpectedTrapExceptionSubCode() {
	return g_observed_exception_subcode;
}

std::uint64_t ExpectedTrapEra() {
	return g_observed_era;
}

std::uint64_t ExpectedTrapBadVaddr() {
	return g_observed_badv;
}

bool HandleTrap(
	TrapFrame* tf,
	std::uint64_t exception_code,
	std::uint64_t exception_subcode,
	std::uint64_t interrupt_status) {
	// LoongArch exceptions are reported via CSR.ESTAT (Exception Status).
	//
	// - Interrupts arrive with EXCCODE=0 and the pending lines in ESTAT.IS.
	// - BREAK uses EXCCODE=0x0c.
	//
	// This handler is intentionally minimal: it only consumes events required
	// by the current test suite. Everything else is escalated to the kernel trap
	// handler so failures are loud and stop-the-world.
	if (exception_code == kExceptionCodeInterrupt &&
		(interrupt_status & kTimerInterruptPendingBit) != 0) {
		Rocinante::Trap::ClearTimerInterrupt();
		Rocinante::Trap::StopTimer();
		g_timer_interrupt_observed = true;
		return true;
	}

	if (exception_code == kExceptionCodeBreak) {
		// Volatile nuance: keep the increment as an explicit load/add/store.
		g_break_trap_count = g_break_trap_count + 1;

		// Skip the BREAK instruction so we can prove ERTN (Exception Return)
		// works.
		//
		// The trap stub returns to CSR.ERA (Exception Return Address). The kernel
		// saves that into TrapFrame::exception_return_address; the assembly stub
		// must copy the updated value back into CSR.ERA before executing ERTN.
		tf->exception_return_address += kInstructionSizeBytes;
		return true;
	}

	if (g_expected_trap_armed) {
		const bool subcode_matches =
			(g_expected_exception_subcode == kAnyExceptionSubcode) ||
			(exception_subcode == g_expected_exception_subcode);
		if (exception_code == g_expected_exception_code && subcode_matches) {
			g_observed_exception_code = exception_code;
			g_observed_exception_subcode = exception_subcode;
			g_observed_era = tf->exception_return_address;
			g_observed_badv = tf->bad_virtual_address;

			g_expected_trap_observed = true;
			g_expected_trap_armed = false;

			// Skip the faulting instruction.
			tf->exception_return_address += kInstructionSizeBytes;
			return true;
		}
	}

	return false;
}

void Fail(TestContext* ctx, const char* file, int line, const char* message) {
	ctx->current_test_failures++;

	Print(ctx, "FAIL [");
	Print(ctx, ctx->current_test_name ? ctx->current_test_name : "<unknown>");
	Print(ctx, "] ");
	Print(ctx, message);
	Print(ctx, " (at ");
	Print(ctx, file);
	Print(ctx, ":");
	PrintU64(ctx, static_cast<std::uint64_t>(line));
	Print(ctx, ")\n");
}

void ExpectTrue(TestContext* ctx, bool value, const char* expr_text, const char* file, int line) {
	if (value) return;
	Fail(ctx, file, line, expr_text);
}

void ExpectEqU64(
	TestContext* ctx,
	std::uint64_t actual,
	std::uint64_t expected,
	const char* actual_text,
	const char* expected_text,
	const char* file,
	int line) {
	if (actual == expected) return;

	ctx->current_test_failures++;

	Print(ctx, "FAIL [");
	Print(ctx, ctx->current_test_name ? ctx->current_test_name : "<unknown>");
	Print(ctx, "] ");
	Print(ctx, actual_text);
	Print(ctx, " != ");
	Print(ctx, expected_text);
	Print(ctx, " (at ");
	Print(ctx, file);
	Print(ctx, ":");
	PrintU64(ctx, static_cast<std::uint64_t>(line));
	Print(ctx, ")\n");

	Print(ctx, "  actual:   ");
	ctx->uart->write_hex_u64(actual);
	Print(ctx, "\n  expected: ");
	ctx->uart->write_hex_u64(expected);
	Print(ctx, "\n");
}

int RunAll(Uart16550* uart) {
	TestContext ctx{.uart = uart};

	uart->puts("\n=== Rocinante Kernel Test Suite ===\n");

	std::uint32_t failed_tests = 0;
	for (std::size_t i = 0; i < g_test_case_count; i++) {
		ctx.current_test_name = g_test_cases[i].name;
		ctx.current_test_failures = 0;

		uart->puts("[TEST] ");
		uart->puts(ctx.current_test_name);
		uart->puts("\n");

		g_test_cases[i].fn(&ctx);

		if (ctx.current_test_failures == 0) {
			uart->puts("[PASS] ");
			uart->puts(ctx.current_test_name);
			uart->puts("\n");
		} else {
			uart->puts("[FAIL] ");
			uart->puts(ctx.current_test_name);
			uart->puts(" (failures=");
			uart->write_dec_u64(ctx.current_test_failures);
			uart->puts(")\n");
			failed_tests++;
			ctx.total_failures += ctx.current_test_failures;
		}
	}

	uart->puts("\n=== Test Summary ===\n");
	uart->puts("Failed test cases: ");
	uart->write_dec_u64(failed_tests);
	uart->putc('\n');
	uart->puts("Total assertion failures: ");
	uart->write_dec_u64(ctx.total_failures);
	uart->putc('\n');

	return static_cast<int>(failed_tests);
}

} // namespace Rocinante::Testing

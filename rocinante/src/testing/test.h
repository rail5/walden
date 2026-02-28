/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante {

class Uart16550;
struct TrapFrame;

namespace Testing {

// Kernel test execution context.
//
// This is intentionally tiny and POD-like so tests can run in a freestanding
// kernel without relying on dynamic allocation, exceptions, or RTTI.
struct TestContext final {
	// Non-owning UART pointer used for all test output.
	// Must outlive the test run.
	Uart16550* uart;

	const char* current_test_name = nullptr;
	std::uint32_t current_test_failures = 0;
	std::uint32_t total_failures = 0;
};

using TestFn = void (*)(TestContext*);

struct TestCase final {
	const char* name;
	TestFn fn;
};

// Linked-in test registry (defined in src/testing/tests.cpp).
//
// Note: these must have external linkage. In a freestanding kernel we avoid
// constructors and registration side effects; instead the registry is a plain
// array that the linker pulls in.
extern const TestCase g_test_cases[];
extern const std::size_t g_test_case_count;

// Runs the linked-in test suite and prints a summary to UART.
//
// Returns: number of failed test cases.
int RunAll(Uart16550* uart);

// Called from the kernel trap handler when ROCINANTE_TESTS is enabled.
//
// Args are derived from the LoongArch exception status CSR:
// - exception_code: EXCCODE field from CSR.ESTAT (Exception Status).
// - exception_subcode: subcode field from CSR.ESTAT for certain exception types.
// - interrupt_status: pending interrupt lines from CSR.ESTAT.IS.
//
// Returns: true if the trap was consumed by the test harness and execution
// should resume.
bool HandleTrap(
	TrapFrame* tf,
	std::uint64_t exception_code,
	std::uint64_t exception_subcode,
	std::uint64_t interrupt_status);

// Trap observations that individual tests can assert on.
void ResetTrapObservations();
std::uint32_t BreakTrapCount();
bool TimerInterruptObserved();

// --- Expected synchronous exception support ---
//
// Some tests intentionally trigger synchronous exceptions and need the trap
// handler to consume them and resume execution (by skipping the faulting
// instruction). This mechanism is opt-in: tests must arm an expected trap.
static constexpr std::uint64_t kAnyExceptionSubcode = ~0ull;

void ArmExpectedTrap(std::uint64_t exception_code, std::uint64_t exception_subcode = kAnyExceptionSubcode);
bool ExpectedTrapObserved();
std::uint64_t ExpectedTrapExceptionCode();
std::uint64_t ExpectedTrapExceptionSubCode();
std::uint64_t ExpectedTrapEra();
std::uint64_t ExpectedTrapBadVaddr();

// --- Assertion helpers ---

// Marks the current test as failed and prints a diagnostic.
//
// This does not stop execution: tests should be able to report multiple
// failures in one run.
void Fail(TestContext* ctx, const char* file, int line, const char* message);

void ExpectTrue(TestContext* ctx, bool value, const char* expr_text, const char* file, int line);

void ExpectEqU64(
	TestContext* ctx,
	std::uint64_t actual,
	std::uint64_t expected,
	const char* actual_text,
	const char* expected_text,
	const char* file,
	int line);

} // namespace Testing

} // namespace Rocinante

#define ROCINANTE_EXPECT_TRUE(ctx, expr) \
	::Rocinante::Testing::ExpectTrue((ctx), static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define ROCINANTE_EXPECT_EQ_U64(ctx, actual, expected) \
	::Rocinante::Testing::ExpectEqU64((ctx), static_cast<std::uint64_t>(actual), static_cast<std::uint64_t>(expected), #actual, #expected, __FILE__, __LINE__)

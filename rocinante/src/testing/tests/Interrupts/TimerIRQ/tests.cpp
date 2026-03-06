/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/trap/trap.h>

#include <cstdint>

namespace Rocinante::Testing {

namespace {

// Read the CPU time counter (LoongArch `rdtime.d`).
//
// Returns: monotonically increasing time-counter ticks.
static inline std::uint64_t ReadTimeCounterTicks() {
	std::uint64_t value;
	asm volatile("rdtime.d %0, $zero" : "=r"(value));
	return value;
}

static void Test_Interrupts_TimerIRQ_DeliversAndClears(TestContext* ctx) {
	ResetTrapObservations();

	Rocinante::Trap::DisableInterrupts();
	Rocinante::Trap::MaskAllInterruptLines();

	// The units here are timer ticks (hardware-defined). The goal is not a
	// precise delay; it is to reliably trigger a timer interrupt in QEMU.
	static constexpr std::uint64_t kOneShotTimerDelayTicks = 100000;
	Rocinante::Trap::StartOneShotTimerTicks(kOneShotTimerDelayTicks);
	Rocinante::Trap::UnmaskTimerInterruptLine();
	Rocinante::Trap::EnableInterrupts();

	// We need a timeout so a broken interrupt path fails loudly instead of
	// hanging the kernel forever.
	//
	// The time counter frequency is platform/QEMU dependent; choose a generous
	// timeout in time-counter ticks.
	static constexpr std::uint64_t kTimeoutTimeCounterTicks = 50000000ull;

	const std::uint64_t start_time_ticks = ReadTimeCounterTicks();
	while (!TimerInterruptObserved()) {
		const std::uint64_t now_ticks = ReadTimeCounterTicks();
		if ((now_ticks - start_time_ticks) > kTimeoutTimeCounterTicks) {
			break;
		}
		asm volatile("nop" ::: "memory");
	}

	Rocinante::Trap::DisableInterrupts();

	ROCINANTE_EXPECT_TRUE(ctx, TimerInterruptObserved());
}

} // namespace

void TestEntry_Interrupts_TimerIRQ_DeliversAndClears(TestContext* ctx) {
	Test_Interrupts_TimerIRQ_DeliversAndClears(ctx);
}

} // namespace Rocinante::Testing

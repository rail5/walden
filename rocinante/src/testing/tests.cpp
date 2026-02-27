/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <cstddef>
#include <cstdint>

#include <src/sp/cpucfg.h>
#include <src/trap.h>

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

struct FakeCPUCFGBackend final {
	// LoongArch CPUCFG currently defines words 0x0..0x14.
	static constexpr std::uint32_t kCPUCFGWordCount = 0x15;

	std::uint32_t words[kCPUCFGWordCount]{};

	static std::uint32_t Read(void* context, std::uint32_t word_number) {
		auto* self = static_cast<FakeCPUCFGBackend*>(context);
		if (word_number < kCPUCFGWordCount) return self->words[word_number];
		return 0;
	}
};

static void Test_CPUCFG_FakeBackend_DecodesWord1(TestContext* ctx) {
	CPUCFG cpucfg;
	FakeCPUCFGBackend fake;

	// Construct CPUCFG word 0x1 using the architectural bit layout.
	//
	// Fields (LoongArch CPUCFG word 1):
	// - ARCH in bits [1:0]
	// - PALEN-1 (physical address bits minus 1) in bits [11:4]
	// - VALEN-1 (virtual address bits minus 1) in bits [19:12]
	static constexpr std::uint32_t kCPUCFGWordIndex = 0x1;
	static constexpr std::uint32_t kArchShift = 0;
	static constexpr std::uint32_t kPhysicalAddressBitsMinus1Shift = 4;
	static constexpr std::uint32_t kVirtualAddressBitsMinus1Shift = 12;

	static constexpr std::uint32_t kArchLA64 = 2;
	static constexpr std::uint32_t kPhysicalAddressBitsMinus1 = 47;
	static constexpr std::uint32_t kVirtualAddressBitsMinus1 = 47;

	static constexpr std::uint32_t kWord1 =
		(kArchLA64 << kArchShift) |
		(kPhysicalAddressBitsMinus1 << kPhysicalAddressBitsMinus1Shift) |
		(kVirtualAddressBitsMinus1 << kVirtualAddressBitsMinus1Shift);

	fake.words[kCPUCFGWordIndex] = kWord1;

	cpucfg.SetBackend(CPUCFGBackend{.context = &fake, .read_word = &FakeCPUCFGBackend::Read});

	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(cpucfg.Arch()), static_cast<std::uint64_t>(CPUCFG::Architecture::LA64));
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.PhysicalAddressBits(), kPhysicalAddressBitsMinus1 + 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.VirtualAddressBits(), kVirtualAddressBitsMinus1 + 1);

	// Word 0x1 should be cached after the first access.
	(void)cpucfg.VirtualAddressBits();
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.BackendReadCount(), 1);
}

static void Test_CPUCFG_FakeBackend_CachesWords(TestContext* ctx) {
	CPUCFG cpucfg;
	FakeCPUCFGBackend fake;

	static constexpr std::uint32_t kCPUCFGWord0Index = 0x0;
	static constexpr std::uint32_t kProcessorIDWordValue = 0x12345678u;

	fake.words[kCPUCFGWord0Index] = kProcessorIDWordValue;
	cpucfg.SetBackend(CPUCFGBackend{.context = &fake, .read_word = &FakeCPUCFGBackend::Read});

	(void)cpucfg.ProcessorID();
	(void)cpucfg.ProcessorID();
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.BackendReadCount(), 1);

	cpucfg.ResetCache();
	(void)cpucfg.ProcessorID();
	ROCINANTE_EXPECT_EQ_U64(ctx, cpucfg.BackendReadCount(), 1);
}

static void Test_Traps_BREAK_EntersAndReturns(TestContext* ctx) {
	ResetTrapObservations();

	// The BREAK instruction causes a synchronous exception.
	//
	// This test proves:
	// - the trap entry path is wired up,
	// - the handler can observe EXCCODE=BREAK, and
	// - the ERTN (Exception Return) path works after we adjust the saved
	//   exception return address.
	asm volatile("break 0" ::: "memory");

	ROCINANTE_EXPECT_EQ_U64(ctx, BreakTrapCount(), 1);
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

extern const TestCase g_test_cases[] = {
	{"CPUCFG.FakeBackend.DecodesWord1", &Test_CPUCFG_FakeBackend_DecodesWord1},
	{"CPUCFG.FakeBackend.CachesWords", &Test_CPUCFG_FakeBackend_CachesWords},
	{"Traps.BREAK.EntersAndReturns", &Test_Traps_BREAK_EntersAndReturns},
	{"Interrupts.TimerIRQ.DeliversAndClears", &Test_Interrupts_TimerIRQ_DeliversAndClears},
};

extern const std::size_t g_test_case_count = sizeof(g_test_cases) / sizeof(g_test_cases[0]);

} // namespace Rocinante::Testing

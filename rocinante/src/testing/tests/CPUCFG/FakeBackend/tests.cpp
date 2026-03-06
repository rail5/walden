/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/sp/cpucfg.h>

#include <cstddef>
#include <cstdint>

namespace Rocinante::Testing {

namespace {

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

} // namespace

void TestEntry_CPUCFG_FakeBackend_DecodesWord1(TestContext* ctx) {
	Test_CPUCFG_FakeBackend_DecodesWord1(ctx);
}

void TestEntry_CPUCFG_FakeBackend_CachesWords(TestContext* ctx) {
	Test_CPUCFG_FakeBackend_CachesWords(ctx);
}

} // namespace Rocinante::Testing

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "trap.h"

#include <cstdint>

namespace {

// CSR numbers and bitfields used here.
//
// Source of truth:
// - LoongArch Privileged Architecture (CSRs)
// Linux LoongArch headers use the same numbering (e.g. LOONGARCH_CSR_ECFG = 0x4)
namespace Csr {
	constexpr std::uint32_t CurrentModeInformation = 0x0;   // CSR.CRMD
	constexpr std::uint32_t ExceptionConfiguration = 0x4;   // CSR.ECFG
	constexpr std::uint32_t ExceptionEntryAddress = 0xC;    // CSR.EENTRY
	constexpr std::uint32_t TlbRefillEntryAddress = 0x88;   // CSR.TLBRENTRY
	constexpr std::uint32_t MachineErrorEntryAddress = 0x93; // CSR.MERRENTRY
	constexpr std::uint32_t TimerConfiguration = 0x41;      // CSR.TCFG
	constexpr std::uint32_t TimerInterruptClear = 0x44;     // CSR.TINTCLR
}

namespace CurrentModeInformation {
	// CRMD.IE (bit 2): Global interrupt enable.
	constexpr std::uint64_t InterruptEnable = (1ull << 2);
}

namespace ExceptionConfiguration {
	// ECFG.IM bit positions (interrupt mask bits). Timer is line 11.
	[[maybe_unused]] constexpr std::uint32_t TimerInterruptLine = 11;
	constexpr std::uint32_t TimerInterruptMaskBit = (1u << TimerInterruptLine);
}

namespace TimerConfiguration {
	// TCFG bitfield:
	// - bit 0: EN     (timer enable)
	// - bit 1: PERIOD (periodic mode)
	// - bits [??:2]: VAL (initial count value)
	constexpr std::uint64_t Enable = 1ull;
	[[maybe_unused]] constexpr std::uint64_t Periodic = (1ull << 1);
	constexpr std::uint32_t InitialValueShift = 2;
}

static inline std::uint64_t ReadCurrentModeInformation() {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(Csr::CurrentModeInformation));
	return value;
}

static inline void WriteCurrentModeInformation(std::uint64_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(Csr::CurrentModeInformation));
}

static inline std::uint32_t ReadExceptionConfiguration() {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(Csr::ExceptionConfiguration));
	return static_cast<std::uint32_t>(value);
}

static inline void WriteExceptionConfiguration(std::uint32_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(Csr::ExceptionConfiguration));
}

static inline void WriteExceptionEntryAddress(std::uint64_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(Csr::ExceptionEntryAddress));
}

static inline void WriteTlbRefillEntryAddress(std::uint64_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(Csr::TlbRefillEntryAddress));
}

static inline void WriteMachineErrorEntryAddress(std::uint64_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(Csr::MachineErrorEntryAddress));
}

static inline void WriteTimerConfiguration(std::uint64_t value) {
	asm volatile("csrwr %0, %1" :: "r"(value), "i"(Csr::TimerConfiguration));
}

static inline void ClearPendingTimerInterruptInCsr() {
	// TINTCLR.TI is bit 0. Writing 1 clears the pending timer interrupt.
	asm volatile("csrwr %0, %1" :: "r"(1ull), "i"(Csr::TimerInterruptClear));
}

extern "C" void __exception_entry();

} // namespace

namespace Rocinante::Trap {

void Initialize() {
	// Use a unified entry point (ECFG.VS = 0).
	//
	// This means all exceptions/interrupts enter through CSR.EENTRY, and the
	// handler can decode `CSR.ESTAT` to determine the cause.
	//
	// We also start with all interrupt lines masked.
	WriteExceptionConfiguration(0);

	const auto entry = reinterpret_cast<std::uint64_t>(&__exception_entry);
	WriteExceptionEntryAddress(entry);
	WriteTlbRefillEntryAddress(entry);
	WriteMachineErrorEntryAddress(entry);
}

void EnableInterrupts() {
	auto current_mode_information = ReadCurrentModeInformation();
	current_mode_information |= CurrentModeInformation::InterruptEnable;
	WriteCurrentModeInformation(current_mode_information);
}

void DisableInterrupts() {
	auto current_mode_information = ReadCurrentModeInformation();
	current_mode_information &= ~CurrentModeInformation::InterruptEnable;
	WriteCurrentModeInformation(current_mode_information);
}

void MaskAllInterruptLines() {
	WriteExceptionConfiguration(0);
}

void UnmaskTimerInterruptLine() {
	// Keep unified entry (VS=0); only unmask the timer interrupt line.
	//
	// Note: IM bits are a mask: 1 means "this interrupt line may be delivered".
	// CRMD.IE still must be enabled for delivery.
	std::uint32_t exception_configuration = ReadExceptionConfiguration();
	exception_configuration |= ExceptionConfiguration::TimerInterruptMaskBit;
	WriteExceptionConfiguration(exception_configuration);
}

void StopTimer() {
	WriteTimerConfiguration(0);
}

void ClearTimerInterrupt() {
	ClearPendingTimerInterruptInCsr();
}

void StartOneShotTimerTicks(std::uint64_t ticks) {
	// One-shot timer:
	// - PERIOD=0
	// - EN=1
	// - VAL=ticks
	StopTimer();
	ClearPendingTimerInterruptInCsr();
	const std::uint64_t timer_configuration =
		(ticks << TimerConfiguration::InitialValueShift) | TimerConfiguration::Enable;
	WriteTimerConfiguration(timer_configuration);
}

} // namespace Rocinante::Trap

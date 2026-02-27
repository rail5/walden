/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante {

/**
 * @brief Saved machine state at exception/interrupt entry.
 *
 * The assembly entry stub constructs one of these on the current stack and
 * passes it to `RocinanteTrapHandler()`.
 *
 * Notes:
 * - `general_purpose_registers[i]` corresponds to LoongArch GPR `r{i}`.
 * - `general_purpose_registers[3]` is saved as the *pre-exception* stack
 *   pointer value (since we adjust `$sp` to allocate this frame).
 * - The other fields are snapshots of key Control and Status Registers (CSRs)
 *   as defined by the LoongArch privileged architecture.
 */
struct TrapFrame final {
	std::uint64_t general_purpose_registers[32];

	// CSR.ERA: Exception Return Address
	std::uint64_t exception_return_address;

	// CSR.ESTAT: Exception Status
	std::uint64_t exception_status;

	// CSR.BADV: Bad Virtual Address
	std::uint64_t bad_virtual_address;

	// CSR.CRMD: Current Mode Information
	std::uint64_t current_mode_information;

	// CSR.PRMD: Previous Mode Information
	std::uint64_t previous_mode_information;

	// CSR.ECFG: Exception Configuration
	std::uint64_t exception_configuration;
};

static_assert(sizeof(TrapFrame) == 304);

namespace Trap {

/**
 * @brief Installs exception/interrupt entry points into the relevant CSRs.
 *
 * Current policy:
 * - Unified (non-vectored) entry: ECFG.VS is set to 0.
 * - All interrupt lines are masked initially. Call `UnmaskTimerInterruptLine()`
 *   (and later other unmask helpers) and `EnableInterrupts()` when ready.
 */
void Initialize();

// Global interrupt enable in CSR.CRMD.
void EnableInterrupts();
void DisableInterrupts();

// --- Minimal timer interrupt bring-up ---
//
// This uses the CPU-local timer exposed via CSRs:
// - CSR.TCFG    (Timer Configuration)
// - CSR.TINTCLR (Timer Interrupt Clear)
//
// The self-tests in `kernel.cpp` can enable these at build time:
// - `ROCINANTE_SELFTEST_TRAPS`
// - `ROCINANTE_SELFTEST_TIMER_IRQ`

// Masks/unmasks interrupt lines via CSR.ECFG (does not toggle CRMD.IE).
void MaskAllInterruptLines();
void UnmaskTimerInterruptLine();

// Programs a one-shot timer and clears any pending timer interrupt.
void StartOneShotTimerTicks(std::uint64_t ticks);

// Stops the timer (disables it in CSR.TCFG).
void StopTimer();

// Clears a pending timer interrupt.
void ClearTimerInterrupt();

// Helpers for decoding CSR.ESTAT (Exception Status).
constexpr std::uint64_t ExceptionCodeFromExceptionStatus(std::uint64_t exception_status) {
	// ESTAT.EXC is bits [21:16].
	return (exception_status >> 16) & 0x3fu;
}

constexpr std::uint64_t ExceptionSubCodeFromExceptionStatus(std::uint64_t exception_status) {
	// ESTAT.ESUBCODE is bits [30:22].
	return (exception_status >> 22) & 0x1ffu;
}

constexpr std::uint64_t InterruptStatusFromExceptionStatus(std::uint64_t exception_status) {
	// ESTAT.IS is bits [14:0].
	return exception_status & 0x7fffu;
}

} // namespace Trap

} // namespace Rocinante

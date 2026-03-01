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

// ABI contract with the assembly trap entry stub (rocinante/src/asm/trap.S).
static constexpr std::size_t kTrapFrameSizeBytes = 304;
static_assert(sizeof(TrapFrame) == kTrapFrameSizeBytes);

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

/**
 * @brief Reprograms the general-exception entry base (CSR.EENTRY) and machine-error
 * entry base (CSR.MERRENTRY).
 *
 * This exists to support higher-half bring-up: once the kernel jumps to its
 * higher-half alias, we can also move general exception entry to the same alias.
 *
 * Spec anchor (LoongArch-Vol1-EN.html):
 * - Section 6.3.1 (Exception Entry): general exceptions use CSR.EENTRY; machine
 *   error uses CSR.MERRENTRY; TLB refill uses CSR.TLBRENTRY.
 *
 * Constraints:
 * - The low 12 bits of CSR.EENTRY/CSR.MERRENTRY are ignored by hardware.
 *   (Entries are effectively 4 KiB aligned.)
 *
 * This function intentionally does not update CSR.TLBRENTRY.
 */
void SetGeneralAndMachineErrorExceptionEntryPageBase(std::uint64_t entry_page_base);

// Global interrupt enable in CSR.CRMD.
void EnableInterrupts();
void DisableInterrupts();

// --- Minimal timer interrupt bring-up ---
//
// This uses the CPU-local timer exposed via CSRs:
// - CSR.TCFG    (Timer Configuration)
// - CSR.TINTCLR (Timer Interrupt Clear)
//
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
	// LoongArch CSR.ESTAT bitfield layout (privileged spec):
	// - ESTAT.EXC is bits [21:16]
	static constexpr std::uint64_t kExceptionCodeShift = 16;
	static constexpr std::uint64_t kExceptionCodeMask = 0x3f;
	return (exception_status >> kExceptionCodeShift) & kExceptionCodeMask;
}

constexpr std::uint64_t ExceptionSubCodeFromExceptionStatus(std::uint64_t exception_status) {
	// - ESTAT.ESUBCODE is bits [30:22]
	static constexpr std::uint64_t kExceptionSubCodeShift = 22;
	static constexpr std::uint64_t kExceptionSubCodeMask = 0x1ff;
	return (exception_status >> kExceptionSubCodeShift) & kExceptionSubCodeMask;
}

constexpr std::uint64_t InterruptStatusFromExceptionStatus(std::uint64_t exception_status) {
	// - ESTAT.IS is bits [14:0]
	static constexpr std::uint64_t kInterruptStatusMask = 0x7fff;
	return exception_status & kInterruptStatusMask;
}

} // namespace Trap

} // namespace Rocinante

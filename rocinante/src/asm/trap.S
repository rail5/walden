/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

.section .text.trap, "ax"

// This file implements the unified exception/interrupt entry stub installed by
// Rocinante::Trap::Initialize() (see src/trap.cpp).
//
// Design goals for early bring-up:
// - Preserve the complete interrupted CPU state (all GPRs + key CSRs).
// - Call into a C++ handler with a pointer to a well-defined TrapFrame.
// - Return to the interrupted context via `ertn`.
//
// Non-goals (yet):
// - Switching from a user stack to a kernel stack on entry.
//   Today all traps occur while running kernel code, so the current stack is
//   already safe to use. When user mode exists, this stub will need to become a
//   proper trampoline that switches stacks before touching memory.
//
// NOTE: Every instruction and offset here is part of an ABI with TrapFrame.
// Keep the layout in lock-step with src/trap.h.

.globl __exception_entry
.type __exception_entry, @function

// -----------------------------------------------------------------------------
// TrapFrame layout (must match Rocinante::TrapFrame in src/trap.h)
// -----------------------------------------------------------------------------
// TrapFrame::general_purpose_registers[32] (32 * 8 = 256 bytes)
.equ TF_GENERAL_PURPOSE_REGISTERS, 0

// TrapFrame CSR snapshots (each 64-bit)
// - exception_return_address   = CSR.ERA
// - exception_status           = CSR.ESTAT
// - bad_virtual_address        = CSR.BADV
// - current_mode_information   = CSR.CRMD
// - previous_mode_information  = CSR.PRMD
// - exception_configuration    = CSR.ECFG
.equ TF_EXCEPTION_RETURN_ADDRESS, 256
.equ TF_EXCEPTION_STATUS,         264
.equ TF_BAD_VIRTUAL_ADDRESS,      272
.equ TF_CURRENT_MODE_INFORMATION, 280
.equ TF_PREVIOUS_MODE_INFORMATION, 288
.equ TF_EXCEPTION_CONFIGURATION,  296

// Total TrapFrame size.
// 304 is deliberately 16-byte aligned so the C++ call sees a correctly aligned
// stack according to the LoongArch psABI.
.equ TF_SIZE, 304

// Backwards-compatible aliases used by the code below.
.equ TF_GPR_BASE, TF_GENERAL_PURPOSE_REGISTERS
.equ TF_ERA,      TF_EXCEPTION_RETURN_ADDRESS
.equ TF_ESTAT,    TF_EXCEPTION_STATUS
.equ TF_BADV,     TF_BAD_VIRTUAL_ADDRESS
.equ TF_CRMD,     TF_CURRENT_MODE_INFORMATION
.equ TF_PRMD,     TF_PREVIOUS_MODE_INFORMATION
.equ TF_ECFG,     TF_EXCEPTION_CONFIGURATION

// -----------------------------------------------------------------------------
// CSRs used by this stub
// -----------------------------------------------------------------------------
// KSave CSRs are privileged scratch registers that we use as a safe temporary
// holding area.
//
// Why we need this:
// - We want TrapFrame::general_purpose_registers[] to contain the *original*
//   values of all GPRs, including $t0/$t1/$t2.
// - But we need temporary registers to compute addresses / offsets while
//   building the TrapFrame.
// - So, we first spill $t0/$t1/$t2 into CSR.KS0..CSR.KS2, then we are free to
//   reuse $t0..$t2 during frame construction, and finally we store the original
//   values into the TrapFrame from CSR.KS*.
.equ CSR_KS0, 0x30  // CSR.KS0
.equ CSR_KS1, 0x31  // CSR.KS1
.equ CSR_KS2, 0x32  // CSR.KS2

.equ CSR_CURRENT_MODE_INFORMATION,  0x0  // CSR.CRMD
.equ CSR_PREVIOUS_MODE_INFORMATION, 0x1  // CSR.PRMD
.equ CSR_EXCEPTION_CONFIGURATION,   0x4  // CSR.ECFG
.equ CSR_EXCEPTION_STATUS,          0x5  // CSR.ESTAT
.equ CSR_EXCEPTION_RETURN_ADDRESS,  0x6  // CSR.ERA
.equ CSR_BAD_VIRTUAL_ADDRESS,       0x7  // CSR.BADV

// Aliases used by the instruction stream below.
.equ CSR_CRMD,  CSR_CURRENT_MODE_INFORMATION
.equ CSR_PRMD,  CSR_PREVIOUS_MODE_INFORMATION
.equ CSR_ECFG,  CSR_EXCEPTION_CONFIGURATION
.equ CSR_ESTAT, CSR_EXCEPTION_STATUS
.equ CSR_ERA,   CSR_EXCEPTION_RETURN_ADDRESS
.equ CSR_BADV,  CSR_BAD_VIRTUAL_ADDRESS

// C++ handler provided by kernel.cpp.
.extern RocinanteTrapHandler

__exception_entry:
	// Preserve $t0/$t1/$t2 *immediately*.
	//
	// The CPU can trap at any instruction, so $t0..$t2 contain arbitrary values.
	// We must not clobber them before recording them into the TrapFrame.
	csrwr   $t0, CSR_KS0
	csrwr   $t1, CSR_KS1
	csrwr   $t2, CSR_KS2

	// Allocate a TrapFrame on the current stack.
	//
	// We do not yet switch stacks on trap entry; therefore this assumes the
	// interrupted context is already using a valid kernel stack.
	addi.d $sp, $sp, -TF_SIZE

	// Save all GPRs.
	//
	// TrapFrame::general_purpose_registers[i] corresponds to LoongArch GPR r{i}.
	// In particular, general_purpose_registers[3] must be the *pre-trap* $sp.
	// Since we just decremented $sp to allocate the frame, we reconstruct the old
	// value as ($sp + TF_SIZE).

	// r0 is architecturally hardwired to zero.
	st.d $zero, $sp, TF_GPR_BASE+0

	// r1..r11.
	// We intentionally do not use any of these registers as scratch before they
	// are saved.
	st.d $ra,   $sp, TF_GPR_BASE+8
	st.d $tp,   $sp, TF_GPR_BASE+16
	// r3 ($sp) should reflect the pre-trap stack pointer.
	addi.d $t0, $sp, TF_SIZE
	st.d $t0,   $sp, TF_GPR_BASE+24
	st.d $a0,   $sp, TF_GPR_BASE+32
	st.d $a1,   $sp, TF_GPR_BASE+40
	st.d $a2,   $sp, TF_GPR_BASE+48
	st.d $a3,   $sp, TF_GPR_BASE+56
	st.d $a4,   $sp, TF_GPR_BASE+64
	st.d $a5,   $sp, TF_GPR_BASE+72
	st.d $a6,   $sp, TF_GPR_BASE+80
	st.d $a7,   $sp, TF_GPR_BASE+88

	// r12..r14 ($t0..$t2) are restored from CSR.KS*.
	csrrd  $t0, CSR_KS0
	st.d   $t0, $sp, TF_GPR_BASE+96
	csrrd  $t0, CSR_KS1
	st.d   $t0, $sp, TF_GPR_BASE+104
	csrrd  $t0, CSR_KS2
	st.d   $t0, $sp, TF_GPR_BASE+112

	// gpr[15]..gpr[31]
	st.d $t3,   $sp, TF_GPR_BASE+120
	st.d $t4,   $sp, TF_GPR_BASE+128
	st.d $t5,   $sp, TF_GPR_BASE+136
	st.d $t6,   $sp, TF_GPR_BASE+144
	st.d $t7,   $sp, TF_GPR_BASE+152
	st.d $t8,   $sp, TF_GPR_BASE+160
	st.d $r21,  $sp, TF_GPR_BASE+168
	st.d $fp,   $sp, TF_GPR_BASE+176
	st.d $s0,   $sp, TF_GPR_BASE+184
	st.d $s1,   $sp, TF_GPR_BASE+192
	st.d $s2,   $sp, TF_GPR_BASE+200
	st.d $s3,   $sp, TF_GPR_BASE+208
	st.d $s4,   $sp, TF_GPR_BASE+216
	st.d $s5,   $sp, TF_GPR_BASE+224
	st.d $s6,   $sp, TF_GPR_BASE+232
	st.d $s7,   $sp, TF_GPR_BASE+240
	st.d $s8,   $sp, TF_GPR_BASE+248

	// Snapshot key CSRs.
	//
	// These are the values most useful for early debugging:
	// - CSR.ERA:   where `ertn` will return to
	// - CSR.ESTAT: encodes exception code + pending interrupt lines
	// - CSR.BADV:  faulting address for address-related exceptions
	// - CSR.CRMD/CSR.PRMD: current/previous privilege & interrupt state
	// - CSR.ECFG:  interrupt line masking configuration
	csrrd  $t0, CSR_ERA
	st.d   $t0, $sp, TF_ERA
	csrrd  $t0, CSR_ESTAT
	st.d   $t0, $sp, TF_ESTAT
	csrrd  $t0, CSR_BADV
	st.d   $t0, $sp, TF_BADV
	csrrd  $t0, CSR_CRMD
	st.d   $t0, $sp, TF_CRMD
	csrrd  $t0, CSR_PRMD
	st.d   $t0, $sp, TF_PRMD
	csrrd  $t0, CSR_ECFG
	st.d   $t0, $sp, TF_ECFG

	// Call the C++ handler: RocinanteTrapHandler(TrapFrame*).
	//
	// Calling convention:
	// - First argument in $a0.
	// - The handler may freely use the stack.
	// - We must assume the handler clobbers caller-saved registers; therefore we
	//   restore all GPRs after it returns.
	move   $a0, $sp
	bl     RocinanteTrapHandler

	// IMPORTANT: `ertn` returns to CSR.ERA (not the value stored in memory).
	//
	// If the handler adjusted TrapFrame::exception_return_address (for example to
	// skip over a BREAK instruction during self-test), we must write that updated
	// address back into CSR.ERA. Otherwise, `ertn` would return to the original
	// trapping instruction and immediately re-enter the handler.
	ld.d   $t0, $sp, TF_ERA
	csrwr  $t0, CSR_ERA

	// Restore GPRs (except $sp, which is restored by popping the TrapFrame).
	ld.d $ra,   $sp, TF_GPR_BASE+8
	ld.d $tp,   $sp, TF_GPR_BASE+16
	ld.d $a0,   $sp, TF_GPR_BASE+32
	ld.d $a1,   $sp, TF_GPR_BASE+40
	ld.d $a2,   $sp, TF_GPR_BASE+48
	ld.d $a3,   $sp, TF_GPR_BASE+56
	ld.d $a4,   $sp, TF_GPR_BASE+64
	ld.d $a5,   $sp, TF_GPR_BASE+72
	ld.d $a6,   $sp, TF_GPR_BASE+80
	ld.d $a7,   $sp, TF_GPR_BASE+88

	ld.d $t0,   $sp, TF_GPR_BASE+96
	ld.d $t1,   $sp, TF_GPR_BASE+104
	ld.d $t2,   $sp, TF_GPR_BASE+112

	ld.d $t3,   $sp, TF_GPR_BASE+120
	ld.d $t4,   $sp, TF_GPR_BASE+128
	ld.d $t5,   $sp, TF_GPR_BASE+136
	ld.d $t6,   $sp, TF_GPR_BASE+144
	ld.d $t7,   $sp, TF_GPR_BASE+152
	ld.d $t8,   $sp, TF_GPR_BASE+160
	ld.d $r21,  $sp, TF_GPR_BASE+168
	ld.d $fp,   $sp, TF_GPR_BASE+176
	ld.d $s0,   $sp, TF_GPR_BASE+184
	ld.d $s1,   $sp, TF_GPR_BASE+192
	ld.d $s2,   $sp, TF_GPR_BASE+200
	ld.d $s3,   $sp, TF_GPR_BASE+208
	ld.d $s4,   $sp, TF_GPR_BASE+216
	ld.d $s5,   $sp, TF_GPR_BASE+224
	ld.d $s6,   $sp, TF_GPR_BASE+232
	ld.d $s7,   $sp, TF_GPR_BASE+240
	ld.d $s8,   $sp, TF_GPR_BASE+248

	// Pop the TrapFrame and return from exception.
	addi.d $sp, $sp, TF_SIZE
	ertn

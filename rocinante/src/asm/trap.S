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

.globl __tlb_refill_entry
.type __tlb_refill_entry, @function

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

// C++ handler provided by kernel.cpp.
.extern RocinanteTrapHandler

.p2align 12
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
	st.d $zero, $sp, TF_GENERAL_PURPOSE_REGISTERS+0

	// r1..r11.
	// We intentionally do not use any of these registers as scratch before they
	// are saved.
	st.d $ra,   $sp, TF_GENERAL_PURPOSE_REGISTERS+8
	st.d $tp,   $sp, TF_GENERAL_PURPOSE_REGISTERS+16
	// r3 ($sp) should reflect the pre-trap stack pointer.
	addi.d $t0, $sp, TF_SIZE
	st.d $t0,   $sp, TF_GENERAL_PURPOSE_REGISTERS+24
	st.d $a0,   $sp, TF_GENERAL_PURPOSE_REGISTERS+32
	st.d $a1,   $sp, TF_GENERAL_PURPOSE_REGISTERS+40
	st.d $a2,   $sp, TF_GENERAL_PURPOSE_REGISTERS+48
	st.d $a3,   $sp, TF_GENERAL_PURPOSE_REGISTERS+56
	st.d $a4,   $sp, TF_GENERAL_PURPOSE_REGISTERS+64
	st.d $a5,   $sp, TF_GENERAL_PURPOSE_REGISTERS+72
	st.d $a6,   $sp, TF_GENERAL_PURPOSE_REGISTERS+80
	st.d $a7,   $sp, TF_GENERAL_PURPOSE_REGISTERS+88

	// r12..r14 ($t0..$t2) are restored from CSR.KS*.
	csrrd  $t0, CSR_KS0
	st.d   $t0, $sp, TF_GENERAL_PURPOSE_REGISTERS+96
	csrrd  $t0, CSR_KS1
	st.d   $t0, $sp, TF_GENERAL_PURPOSE_REGISTERS+104
	csrrd  $t0, CSR_KS2
	st.d   $t0, $sp, TF_GENERAL_PURPOSE_REGISTERS+112

	// gpr[15]..gpr[31]
	st.d $t3,   $sp, TF_GENERAL_PURPOSE_REGISTERS+120
	st.d $t4,   $sp, TF_GENERAL_PURPOSE_REGISTERS+128
	st.d $t5,   $sp, TF_GENERAL_PURPOSE_REGISTERS+136
	st.d $t6,   $sp, TF_GENERAL_PURPOSE_REGISTERS+144
	st.d $t7,   $sp, TF_GENERAL_PURPOSE_REGISTERS+152
	st.d $t8,   $sp, TF_GENERAL_PURPOSE_REGISTERS+160
	st.d $r21,  $sp, TF_GENERAL_PURPOSE_REGISTERS+168
	st.d $fp,   $sp, TF_GENERAL_PURPOSE_REGISTERS+176
	st.d $s0,   $sp, TF_GENERAL_PURPOSE_REGISTERS+184
	st.d $s1,   $sp, TF_GENERAL_PURPOSE_REGISTERS+192
	st.d $s2,   $sp, TF_GENERAL_PURPOSE_REGISTERS+200
	st.d $s3,   $sp, TF_GENERAL_PURPOSE_REGISTERS+208
	st.d $s4,   $sp, TF_GENERAL_PURPOSE_REGISTERS+216
	st.d $s5,   $sp, TF_GENERAL_PURPOSE_REGISTERS+224
	st.d $s6,   $sp, TF_GENERAL_PURPOSE_REGISTERS+232
	st.d $s7,   $sp, TF_GENERAL_PURPOSE_REGISTERS+240
	st.d $s8,   $sp, TF_GENERAL_PURPOSE_REGISTERS+248

	// Snapshot key CSRs.
	//
	// These are the values most useful for early debugging:
	// - CSR.ERA:   where `ertn` will return to
	// - CSR.ESTAT: encodes exception code + pending interrupt lines
	// - CSR.BADV:  faulting address for address-related exceptions
	// - CSR.CRMD/CSR.PRMD: current/previous privilege & interrupt state
	// - CSR.ECFG:  interrupt line masking configuration
	csrrd  $t0, CSR_EXCEPTION_RETURN_ADDRESS
	st.d   $t0, $sp, TF_EXCEPTION_RETURN_ADDRESS
	csrrd  $t0, CSR_EXCEPTION_STATUS
	st.d   $t0, $sp, TF_EXCEPTION_STATUS
	csrrd  $t0, CSR_BAD_VIRTUAL_ADDRESS
	st.d   $t0, $sp, TF_BAD_VIRTUAL_ADDRESS
	csrrd  $t0, CSR_CURRENT_MODE_INFORMATION
	st.d   $t0, $sp, TF_CURRENT_MODE_INFORMATION
	csrrd  $t0, CSR_PREVIOUS_MODE_INFORMATION
	st.d   $t0, $sp, TF_PREVIOUS_MODE_INFORMATION
	csrrd  $t0, CSR_EXCEPTION_CONFIGURATION
	st.d   $t0, $sp, TF_EXCEPTION_CONFIGURATION

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
	ld.d   $t0, $sp, TF_EXCEPTION_RETURN_ADDRESS
	csrwr  $t0, CSR_EXCEPTION_RETURN_ADDRESS

	// Restore GPRs (except $sp, which is restored by popping the TrapFrame).
	ld.d $ra,   $sp, TF_GENERAL_PURPOSE_REGISTERS+8
	ld.d $tp,   $sp, TF_GENERAL_PURPOSE_REGISTERS+16
	ld.d $a0,   $sp, TF_GENERAL_PURPOSE_REGISTERS+32
	ld.d $a1,   $sp, TF_GENERAL_PURPOSE_REGISTERS+40
	ld.d $a2,   $sp, TF_GENERAL_PURPOSE_REGISTERS+48
	ld.d $a3,   $sp, TF_GENERAL_PURPOSE_REGISTERS+56
	ld.d $a4,   $sp, TF_GENERAL_PURPOSE_REGISTERS+64
	ld.d $a5,   $sp, TF_GENERAL_PURPOSE_REGISTERS+72
	ld.d $a6,   $sp, TF_GENERAL_PURPOSE_REGISTERS+80
	ld.d $a7,   $sp, TF_GENERAL_PURPOSE_REGISTERS+88

	ld.d $t0,   $sp, TF_GENERAL_PURPOSE_REGISTERS+96
	ld.d $t1,   $sp, TF_GENERAL_PURPOSE_REGISTERS+104
	ld.d $t2,   $sp, TF_GENERAL_PURPOSE_REGISTERS+112

	ld.d $t3,   $sp, TF_GENERAL_PURPOSE_REGISTERS+120
	ld.d $t4,   $sp, TF_GENERAL_PURPOSE_REGISTERS+128
	ld.d $t5,   $sp, TF_GENERAL_PURPOSE_REGISTERS+136
	ld.d $t6,   $sp, TF_GENERAL_PURPOSE_REGISTERS+144
	ld.d $t7,   $sp, TF_GENERAL_PURPOSE_REGISTERS+152
	ld.d $t8,   $sp, TF_GENERAL_PURPOSE_REGISTERS+160
	ld.d $r21,  $sp, TF_GENERAL_PURPOSE_REGISTERS+168
	ld.d $fp,   $sp, TF_GENERAL_PURPOSE_REGISTERS+176
	ld.d $s0,   $sp, TF_GENERAL_PURPOSE_REGISTERS+184
	ld.d $s1,   $sp, TF_GENERAL_PURPOSE_REGISTERS+192
	ld.d $s2,   $sp, TF_GENERAL_PURPOSE_REGISTERS+200
	ld.d $s3,   $sp, TF_GENERAL_PURPOSE_REGISTERS+208
	ld.d $s4,   $sp, TF_GENERAL_PURPOSE_REGISTERS+216
	ld.d $s5,   $sp, TF_GENERAL_PURPOSE_REGISTERS+224
	ld.d $s6,   $sp, TF_GENERAL_PURPOSE_REGISTERS+232
	ld.d $s7,   $sp, TF_GENERAL_PURPOSE_REGISTERS+240
	ld.d $s8,   $sp, TF_GENERAL_PURPOSE_REGISTERS+248

	// Pop the TrapFrame and return from exception.
	addi.d $sp, $sp, TF_SIZE
	ertn

// -----------------------------------------------------------------------------
// TLB refill exception entry
// -----------------------------------------------------------------------------
// LoongArch software-managed TLB refill.
//
// Key properties (LoongArch-Vol1-EN.html, Section 5.4.3.1):
// - TLB refill exception uses CSR.TLBRENTRY.
// - While it is being caught, hardware sets CRMD.DA=1 and CRMD.PG=0 (direct
//   addressing), and sets CSR.TLBRERA.IsTLBR=1.
// - A separate set of CSRs is used as the TLB access interface in this context.
//
// This stub performs a minimal 4 KiB page walk to obtain the leaf PTE table
// base, then uses:
// - LDPTE: load the PTE (even/odd) into CSR.TLBRELO0/1
//
// NOTE:
// The spec's LDDIR description has proven ambiguous during QEMU bring-up.
// This handler therefore does an explicit software walk (ld.d directory
// entries + 4 KiB masking) driven by PWCL/PWCH, and avoids relying on LDDIR.
//
// It then fills the TLB via TLBFILL and returns via ERTN.
//
// Bring-up constraints / assumptions:
// - Kernel currently runs from low physical addresses and uses a stack that is
//   valid in direct-addressing mode.
// - Page tables are 4 KiB, 64-bit entries, and PWCL/PWCH have been configured
//   to match the page-table shape.

// Page-walk control CSRs (LoongArch-Vol1-EN.html, Section 7.5.8 / 7.5.9).
// We use these to determine which directory levels exist (width != 0).
.equ CSR_PWCL, 0x1C // CSR.PWCL
.equ CSR_PWCH, 0x1D // CSR.PWCH

// Common bring-up constants.
//
// NOTE: Paging bring-up currently assumes 4 KiB base pages.
.equ U64_HEX_NIBBLES, 16
.equ NIBBLE_MASK, 0xF

.equ PAGE_SHIFT_BITS, 12
.equ PAGE_OFFSET_MASK, 0xFFF
.equ PAGE_BASE_MASK, -4096 // == ~PAGE_OFFSET_MASK (mask to 4 KiB-aligned base)

.equ PTE_INDEX_MASK_4K, 0x1FF // 9-bit index (512 entries) for 4 KiB paging
.equ PTE_ENTRY_SHIFT_BYTES, 3 // 8-byte entries

// QEMU virt bring-up guard: avoid dereferencing addresses outside guest RAM.
.equ QEMU_VIRT_RAM_SIZE_BYTES, 0x10000000 // 256 MiB

.equ CSR_TLBIDX, 0x10 // CSR.TLBIDX
.equ CSR_TLBEHI, 0x11 // CSR.TLBEHI
.equ CSR_TLBELO0, 0x12 // CSR.TLBELO0
.equ CSR_TLBELO1, 0x13 // CSR.TLBELO1
.equ CSR_PGD, 0x1B // CSR.PGD (read-only effective root page directory)
.equ UART16550_BASE, 0x1fe001e0 // QEMU LoongArch virt UART16550 base (physical address).
// Used only for very early bring-up breadcrumbs in the TLB refill handler.

.equ CSR_TLBRBADV, 0x89 // CSR.TLBRBADV (TLB refill bad virtual address)

.equ CSR_TLBRSAVE, 0x8B // CSR.TLBRSAVE (TLB refill exception data save register)

.equ CSR_TLBREHI, 0x8E // CSR.TLBREHI
.equ CSR_TLBRELO0, 0x8C // CSR.TLBRELO0
.equ CSR_TLBRELO1, 0x8D // CSR.TLBRELO1

#if defined(ROCINANTE_PAGING_BRINGUP)
.section .rodata
.p2align 4
__rocinante_hex_digits:
	.ascii "0123456789abcdef"
.section .text.trap, "ax"
#endif

// Dedicated TLB refill stack (direct-addressing-safe).
//
// Spec anchor: LoongArch-Vol1-EN.html, Section 6.3.4.
// During TLBR handling, hardware sets CRMD.DA=1 and CRMD.PG=0, so the refill
// handler must not touch the interrupted stack if it contains a virtual
// address (e.g. higher-half kernel stack).
.section .bss
.p2align 12
__tlb_refill_stack:
	.skip 4096
__tlb_refill_stack_top:
	.p2align 3
__tlb_refill_debug_flags:
	.skip 8
.section .text.trap, "ax"

.p2align 12
__tlb_refill_entry:
	// TLB refill exception entry runs with CRMD.DA=1 and CRMD.PG=0 (direct
	// addressing). If the interrupted $sp is a higher-half virtual address, any
	// attempt to push to it will fault.
	//
	// We save one scratch GPR into CSR.TLBRSAVE, then use it to compute a known
	// low physical refill stack frame and spill the remaining temporaries (and
	// the interrupted $sp) there before doing any further memory access.
	//
	// Spec anchors:
	// - LoongArch-Vol1-EN.html, Section 6.3.4 (DA=1, PG=0 during TLBR handling)
	// - LoongArch-Vol1-EN.html, Section 7.5.14 (CSR.TLBRSAVE)
	// NOTE: Do not use CSR.KS* here.
	// TLBR can trigger while other exception handlers are running, and those
	// handlers may already be using CSR.KS* for their own save/restore paths.
	// Use CSR.TLBRSAVE (TLBR-specific) plus a dedicated low refill stack.
	csrwr  $t0, CSR_TLBRSAVE
	la.local $t0, __tlb_refill_stack_top
	addi.d $t0, $t0, -48
	st.d   $sp, $t0, 0
	st.d   $t1, $t0, 8
	st.d   $t2, $t0, 16
	st.d   $t3, $t0, 24
	st.d   $t4, $t0, 32
	st.d   $t5, $t0, 40
	move   $sp, $t0

// NOTE:
// These UART breadcrumbs are extremely noisy once paging is enabled and TLBR
// starts firing frequently (they can interleave with normal UART output).
// Keep them available for deep paging bring-up, but default them off.
//
// Enable by defining ROCINANTE_TLBR_UART_BREADCRUMBS at build time.
#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	// Optional single-shot diagnostics.
	//
	// Use a BSS flag (direct-addressing-safe) to avoid flooding UART.
	// - $t4 acts as a per-invocation boolean: 1 => print, 0 => skip.
	la.local $t0, __tlb_refill_debug_flags
	ld.d   $t1, $t0, 0
	andi   $t2, $t1, 0x2
	bnez   $t2, 0f
	ori    $t1, $t1, 0x2
	st.d   $t1, $t0, 0
	li.d   $t4, 1
	b      1f
0:
	move   $t4, $zero
1:

	// UART breadcrumb: entered TLB refill handler.
	li.d   $t3, UART16550_BASE
	li.w   $t2, 'A'
	st.b   $t2, $t3, 0

	// UART breadcrumb (single-shot): print TLBRBADV as 16 hex digits.
	//
	// Why single-shot:
	// - A broken refill path loops extremely fast.
	// - Printing the full faulting VA on every iteration floods UART and makes
	//   other breadcrumbs unreadable.
	//
	// We use CSR.KS0 as a debug-only "have printed TLBRBADV" flag.
	la.local $t0, __tlb_refill_debug_flags
	ld.d   $t1, $t0, 0
	andi   $t2, $t1, 0x1
	bnez   $t2, 1f
	ori    $t1, $t1, 0x1
	st.d   $t1, $t0, 0

	li.w   $t2, 'B'
	st.b   $t2, $t3, 0
	csrrd  $t1, CSR_TLBRBADV
	la.local $t4, __rocinante_hex_digits
	li.w   $t0, U64_HEX_NIBBLES
0:
	addi.w $t0, $t0, -1
	slli.w $t2, $t0, 2                // shift = (remaining_nibbles-1) * 4
	srl.d  $t2, $t1, $t2
	andi   $t2, $t2, NIBBLE_MASK
	add.d  $t2, $t4, $t2
	ld.bu  $t2, $t2, 0
	st.b   $t2, $t3, 0
	bnez   $t0, 0b

	// Also print CSR.TLBREHI (the refill-context TLBEHI equivalent).
	li.w   $t2, 'H'
	st.b   $t2, $t3, 0
	csrrd  $t1, CSR_TLBREHI
	li.w   $t0, U64_HEX_NIBBLES
2:
	addi.w $t0, $t0, -1
	slli.w $t2, $t0, 2                // shift = (remaining_nibbles-1) * 4
	srl.d  $t2, $t1, $t2
	andi   $t2, $t2, NIBBLE_MASK
	add.d  $t2, $t4, $t2
	ld.bu  $t2, $t2, 0
	st.b   $t2, $t3, 0
	bnez   $t0, 2b
	li.w   $t2, ' '
	st.b   $t2, $t3, 0
1:
#endif

	// Ensure TLBFILL writes a valid entry.
	//
	// Per the spec, TLBFILL consults CSR.TLBIDX.NE to decide whether the
	// populated entry is valid. Clearing NE avoids an infinite refill loop
	// if some earlier path left NE=1.
	//
	// We also set PS=0xC (4 KiB) for our current base-page bring-up.
	// NOTE: The privileged spec has TLBR-context vs non-TLBR-context ambiguity
	// around which CSRs TLBFILL consults; we set PS defensively here.
	// NOTE: Don't clobber the implementation-defined Index field.
	// Spec: LoongArch-Vol1-EN.html, "Definition of TLB index register" (Table 33)
	// - PS is bits [29:24]
	// - NE is bit [31]
	csrrd  $t1, CSR_TLBIDX
	li.d   $t0, 0xbf000000            // (0x3f<<24) | (1<<31)
	nor    $t0, $t0, $zero            // ~0xbf000000
	and    $t1, $t1, $t0              // clear PS and NE
	li.d   $t0, 0x0c000000            // PS=0x0c (4 KiB)
	or     $t1, $t1, $t0
	csrwr  $t1, CSR_TLBIDX

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	// UART breadcrumb: configured TLBIDX.
	li.d   $t3, UART16550_BASE
	li.w   $t2, 'I'
	st.b   $t2, $t3, 0
#endif

	// Start from the effective page-table root.
	csrrd  $t0, CSR_PGD

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	// UART breadcrumb: loaded effective page-table root.
	li.d   $t3, UART16550_BASE
	li.w   $t2, 'R'
	st.b   $t2, $t3, 0
#endif

	// Walk directory levels to obtain the PTE table base.
	//
	// IMPORTANT:
	// We avoid LDDIR for the "real" walk here.
	// The spec's LDDIR level numbering text has internal inconsistencies, and
	// QEMU bring-up has shown non-advancing behavior when using a single guessed
	// mapping. Instead, we do an explicit software walk:
	// - Read PWCL/PWCH base+width fields
	// - Compute indices from CSR.TLBRBADV
	// - Load directory entries with ld.d and mask to 4 KiB alignment
	//
	// This is still spec-driven (PWCL/PWCH define the shape) and removes the
	// dependency on LDDIR's level immediate semantics.

	csrrd  $t1, CSR_TLBRBADV

	// Highest directories in PWCH.
	csrrd  $t2, CSR_PWCH

	// Dir4 (PWCH): width bits [23:18], base bits [17:12]
	srli.d $t5, $t2, 18
	andi   $t5, $t5, 0x3F            // width
	beqz   $t5, 1f
	srli.d $t3, $t2, 12
	andi   $t3, $t3, 0x3F            // base
	srl.d  $t3, $t1, $t3            // idx = badv >> base
	li.d   $t2, 1
	sll.d  $t2, $t2, $t5            // (1<<width)
	addi.d $t2, $t2, -1              // mask
	and    $t3, $t3, $t2
	slli.d $t3, $t3, 3               // idx * 8
	add.d  $t3, $t0, $t3
	ld.d   $t3, $t3, 0
	li.d   $t0, PAGE_BASE_MASK
	and    $t0, $t3, $t2
1:

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	li.d   $t3, UART16550_BASE
	li.w   $t2, '4'
	st.b   $t2, $t3, 0
#endif

	// Dir3 (PWCH): width bits [11:6], base bits [5:0]
	csrrd  $t2, CSR_PWCH
	srli.d $t5, $t2, 6
	andi   $t5, $t5, 0x3F            // width
	beqz   $t5, 2f
	andi   $t3, $t2, 0x3F            // base
	srl.d  $t3, $t1, $t3
	li.d   $t2, 1
	sll.d  $t2, $t2, $t5
	addi.d $t2, $t2, -1
	and    $t3, $t3, $t2
	slli.d $t3, $t3, 3
	add.d  $t3, $t0, $t3
	ld.d   $t3, $t3, 0
	li.d   $t2, PAGE_BASE_MASK
	and    $t0, $t3, $t2
2:

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	li.d   $t3, UART16550_BASE
	li.w   $t2, '3'
	st.b   $t2, $t3, 0
#endif

	// Lower directories in PWCL.
	csrrd  $t2, CSR_PWCL

	// Dir2 (PWCL): width bits [29:25], base bits [24:20]
	srli.d $t5, $t2, 25
	andi   $t5, $t5, 0x1F            // width
	beqz   $t5, 3f
	srli.d $t3, $t2, 20
	andi   $t3, $t3, 0x1F            // base
	srl.d  $t3, $t1, $t3
	li.d   $t2, 1
	sll.d  $t2, $t2, $t5
	addi.d $t2, $t2, -1
	and    $t3, $t3, $t2
	slli.d $t3, $t3, 3
	add.d  $t3, $t0, $t3
	ld.d   $t3, $t3, 0
	li.d   $t2, PAGE_BASE_MASK
	and    $t0, $t3, $t2
3:

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	li.d   $t3, UART16550_BASE
	li.w   $t2, '2'
	st.b   $t2, $t3, 0
#endif

	// Dirl (PWCL): width bits [19:15], base bits [14:10]
	csrrd  $t2, CSR_PWCL
	srli.d $t5, $t2, 15
	andi   $t5, $t5, 0x1F            // width
	beqz   $t5, 4f
	srli.d $t3, $t2, 10
	andi   $t3, $t3, 0x1F            // base
	srl.d  $t3, $t1, $t3
	li.d   $t2, 1
	sll.d  $t2, $t2, $t5
	addi.d $t2, $t2, -1
	and    $t3, $t3, $t2
	slli.d $t3, $t3, 3
	add.d  $t3, $t0, $t3
	ld.d   $t3, $t3, 0
	li.d   $t2, PAGE_BASE_MASK
	and    $t0, $t3, $t2
4:

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	li.d   $t3, UART16550_BASE
	li.w   $t2, '1'
	st.b   $t2, $t3, 0
#endif

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	// Single-shot diagnostic: dump the raw even/odd PTE values from the leaf
	// PTE table page we just reached.
	//
	// This helps distinguish:
	// - software builder bug (PTE entry is actually 0/unmapped), vs
	// - encoding mismatch (raw PTE looks fine but LDPTE yields invalid ELO).
	beqz   $t4, pte_dump_done
	move   $t5, $t0                  // preserve leaf PTE table base
	li.d   $t3, UART16550_BASE
	la.local $t4, __rocinante_hex_digits

	// Print leaf PTE base: p<16hex>
	li.w   $t1, 'p'
	st.b   $t1, $t3, 0
	move   $t1, $t5
	li.w   $t0, U64_HEX_NIBBLES
pte_base_loop:
	addi.w $t0, $t0, -1
	slli.w $t2, $t0, 2
	srl.d  $t2, $t1, $t2
	andi   $t2, $t2, NIBBLE_MASK
	add.d  $t2, $t4, $t2
	ld.bu  $t2, $t2, 0
	st.b   $t2, $t3, 0
	bnez   $t0, pte_base_loop
	li.w   $t2, ' '
	st.b   $t2, $t3, 0

	// Safety: only dereference the leaf base if it is within the QEMU RAM
	// window (256 MiB). Otherwise we can fault again inside the TLBR handler.
	li.d   $t2, QEMU_VIRT_RAM_SIZE_BYTES
	sltu   $t2, $t5, $t2             // $t2 = ($t5 < 256MiB)
	bnez   $t2, 0f
	li.w   $t1, '!'
	st.b   $t1, $t3, 0
	li.w   $t2, ' '
	st.b   $t2, $t3, 0
	b      pte_dump_done
0:

	// Compute PTE index = (TLBRBADV >> PAGE_SHIFT_BITS) & PTE_INDEX_MASK_4K.
	csrrd  $t1, CSR_TLBRBADV
	srli.d $t2, $t1, PAGE_SHIFT_BITS
	andi   $t2, $t2, PTE_INDEX_MASK_4K
	slli.d $t2, $t2, PTE_ENTRY_SHIFT_BYTES // offset = index * 8
	add.d  $t2, $t5, $t2             // $t2 = &pte[index]

	// Raw even PTE: e<16hex>
	li.w   $t1, 'e'
	st.b   $t1, $t3, 0
	ld.d   $t1, $t2, 0
	li.w   $t0, U64_HEX_NIBBLES
pte_even_loop:
	addi.w $t0, $t0, -1
	slli.w $t2, $t0, 2
	srl.d  $t2, $t1, $t2
	andi   $t2, $t2, NIBBLE_MASK
	add.d  $t2, $t4, $t2
	ld.bu  $t2, $t2, 0
	st.b   $t2, $t3, 0
	bnez   $t0, pte_even_loop
	li.w   $t2, ' '
	st.b   $t2, $t3, 0

	// Raw odd PTE: o<16hex>
	// Recompute &pte[index] because $t2 was used as scratch above.
	csrrd  $t1, CSR_TLBRBADV
	srli.d $t2, $t1, PAGE_SHIFT_BITS
	andi   $t2, $t2, PTE_INDEX_MASK_4K
	slli.d $t2, $t2, PTE_ENTRY_SHIFT_BYTES
	add.d  $t2, $t5, $t2
	li.w   $t1, 'o'
	st.b   $t1, $t3, 0
	ld.d   $t1, $t2, 8
	li.w   $t0, U64_HEX_NIBBLES
pte_odd_loop:
	addi.w $t0, $t0, -1
	slli.w $t2, $t0, 2
	srl.d  $t2, $t1, $t2
	andi   $t2, $t2, NIBBLE_MASK
	add.d  $t2, $t4, $t2
	ld.bu  $t2, $t2, 0
	st.b   $t2, $t3, 0
	bnez   $t0, pte_odd_loop
	li.w   $t2, ' '
	st.b   $t2, $t3, 0

	move   $t0, $t5                  // restore leaf base for LDPTE

pte_dump_done:
#endif

	// Load both halves of the dual-page TLB entry.
	ldpte  $t0, 0
	ldpte  $t0, 1

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	// UART breadcrumbs: did LDPTE fetch *present* PTEs?
	// Print 'v' if V=1, '0' if V=0 for each half.
	li.d   $t3, UART16550_BASE
	csrrd  $t1, CSR_TLBRELO0
	andi   $t1, $t1, 0x1
	beqz   $t1, 5f
	li.w   $t2, 'v'
	b      6f
5:
	li.w   $t2, '0'
6:
	st.b   $t2, $t3, 0
	csrrd  $t1, CSR_TLBRELO1
	andi   $t1, $t1, 0x1
	beqz   $t1, 7f
	li.w   $t2, 'v'
	b      8f
7:
	li.w   $t2, '0'
8:
	st.b   $t2, $t3, 0
#endif

	// In TLBR context, LDPTE updates CSR.TLBRELO0/1.
	// The privileged spec is internally inconsistent about whether TLBFILL consumes
	// TLBELO0/1 or TLBRELO0/1 while CSR.TLBRERA.IsTLBR=1.
	// To avoid filling stale data, mirror the freshly-walked refill ELOs into the
	// non-refill ELOs before TLBFILL.
	csrrd  $t1, CSR_TLBREHI
	csrwr  $t1, CSR_TLBEHI
	csrrd  $t1, CSR_TLBRELO0
	csrwr  $t1, CSR_TLBELO0
	csrrd  $t1, CSR_TLBRELO1
	csrwr  $t1, CSR_TLBELO1

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	li.d   $t3, UART16550_BASE
	li.w   $t2, 'P'
	st.b   $t2, $t3, 0
#endif

	// Fill a TLB entry selected by hardware.
	tlbfill

#if defined(ROCINANTE_PAGING_BRINGUP) && defined(ROCINANTE_TLBR_UART_BREADCRUMBS)
	li.d   $t3, UART16550_BASE
	li.w   $t2, 'F'
	st.b   $t2, $t3, 0
#endif

	// Restore temporaries and return from refill.
	ld.d   $t0, $sp, 0
	ld.d   $t1, $sp, 8
	ld.d   $t2, $sp, 16
	ld.d   $t3, $sp, 24
	ld.d   $t4, $sp, 32
	ld.d   $t5, $sp, 40
	move   $sp, $t0
	csrrd  $t0, CSR_TLBRSAVE
	ertn

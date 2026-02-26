/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

// Put in executable+allocatable section for the linker to find and set as the entry point.
.section .text.start, "ax"
// Export _start as the entry symbol and mark it as a function for the assembler and linker.
.globl _start
.type _start, @function
// kernel_main is defined in C++
.extern kernel_main

// Entry point
_start:
	// QEMU LoongArch64 virt direct-kernel boot jumps here with a0/a1/a2 set.

	// Save the incoming arguments into registers for later use
	// Probably not necessary, but future-proofing in case we later need to do something before calling kernel_main that would clobber the argument registers.
	move    $s0, $a0
	move    $s1, $a1
	move    $s2, $a2

	// Initialize the stack pointer to the top of a statically reserved stack in .bss
	la      $sp, stack_top

	// Now that we have a stack, we can call C++ code. Move the original arguments back into the argument registers and call kernel_main.
	move    $a0, $s0
	move    $a1, $s1
	move    $a2, $s2

	// Call kernel_main. This will not return; if it does, we'll just loop forever.
	bl      kernel_main

1:
	// If we somehow get here (i.e., kernel_main returns), just loop forever to prevent falling off into the void.
	b       1b

.section .bss.stack, "aw", @nobits
.align 16
stack_bottom:
	.space 16384
stack_top:

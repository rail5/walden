# Walden / Rocinante Code Quality Standards

Correctness and debuggability beat cleverness.

These guidelines capture the current project standards. They will evolve.

Walden / Rocinante are compiled with the **C++23** standard with GNU extensions enabled (`-std=gnu++23`). Make good use of modern C++ features, but prefer simplicity and clarity over cleverness.

## Core Principles

The big 3:

- **Lower-level -> more detail.**
  - The lower-level the code, the more comments and explicitness it should have.
  - Once we start writing code that deals directly with CPU registers, exception handling, boot protocols, etc., we should be *more* explicit and *more* verbose than in any other layer, not less.
- **Write code to be read.**
  - "Code" does *not* mean "encoded."
  - You should write code the way you write an essay: with the reader in mind. The reader may be you in 6 months, or a new contributor who has never worked on low-level code before.
  - Assume the reader is intelligent but does not have the context you have in your head right now.
- **Highlight flaws.**
  - The flaws should be the most obvious thing in the code. Do not obscure them in order to save your pride -- your pride doesn't matter.
  - If the real reason for something is "I don't understand this architecture as well as I'd like to," then **say that.**
  - The goal isn't to look good, the goal is to build a system.

Further:

- **Be explicit and self-documenting.**
  - Prefer clear names and named constants over abbreviations and "obvious to me" shortcuts.
- **Avoid magic numbers.**
  - If a value comes from the architecture, hardware, ABI, or a spec: give it a name and explain it.
- **Preserve invariants.**
  - A lot of this code is ABI-sensitive (assembly -- C++ struct layout, linker script expectations, boot protocol). Make those invariants visible and hard to accidentally break.
- **Keep changes minimal and focused.**
  - Don’t refactor unrelated code in the same change.

## Documentation Expectations (especially low-level)

- **Assembly files require "why" comments.**
  - If an instruction sequence is subtle, explain the reason and the consequence of getting it wrong.
  - Use [rocinante/src/asm/start.S](rocinante/src/asm/start.S) or [rocinante/src/asm/trap.S](rocinante/src/asm/trap.S) as the baseline for comment depth/style.
- **Architecture acronyms must be expanded.**
  - If you use `ERA`, `ESTAT`, `CRMD`, etc., include a short mapping to the CSR name and role nearby.
  - Prefer using expanded names in C++ code (e.g., `exception_return_address` instead of `ERA`) in the vast majority of cases.
- **Public interfaces should be Doxygen-friendly.**
  - Headers should document intent, inputs/outputs, invariants, and ordering constraints.

## Naming & Style

- **Prefer long, descriptive identifiers** over abbreviations in C++ APIs.
  - Good: `exception_return_address`, `MaskAllInterruptLines()`, `StartOneShotTimerTicks()`
  - Avoid: `era`, `ecfg`, `Init()` (unless the scope already makes it unambiguous)
- **Names should encode units.** If something is in ticks/bytes/pages, say so (e.g., `ticks`, `bytes`, `page_count`).
- **Use fixed-width types for HW/ABI values:** `std::uint32_t`, `std::uint64_t`, `uintptr_t`.
- **Keep formatting consistent with existing code.** The current codebase uses tabs for indentation and same-line braces.

## Magic Numbers Policy

When you are tempted to write a literal constant:

1. **If it is architectural/spec-defined** (CSR numbers, bit positions, exception codes, MMIO offsets):
   - define a named constant (`constexpr` in C++, `.equ` in assembly), and
   - add a short comment describing what it is and where it comes from.
2. **If it is an ABI/layout constant** (struct offsets, frame sizes):
   - keep it centralized,
   - add `static_assert(sizeof(...))` / alignment checks on the C++ side where possible,
   - document that it is part of an ABI contract.
3. **If it is a temporary bring-up constant** (timeouts, test delays):
   - keep it obviously “bring-up only” and easy to adjust,
   - annotate the assumption (clock rate, expected latency, etc.).

## C++ in a Freestanding Kernel

- **Assume freestanding constraints.**
  No OS, no libc expectations. Avoid accidental dependencies.
- **Exceptions/RTTI are off** (see build flags). Don’t introduce code that relies on them.
- **Prefer simple, auditable constructs** over template metaprogramming.
- **Be careful with static initialization.** If you add globals with constructors, be intentional about init order.

## Assembly Guidelines

- **Treat assembly -- C++ boundaries as an ABI.**
  - If an assembly routine builds a C++ struct (e.g., `TrapFrame`), keep field offsets and size synchronized and clearly documented.
- **Use `.equ` for constants** (CSR numbers, offsets, sizes, bit positions). Add descriptive names and short comments.
- **Document register-save policy.**
  - If you temporarily clobber a register, either save/restore it or explicitly justify why it is safe.
- **Call out subtle architectural behavior explicitly.**
  - Example: `ertn` returns to `CSR.ERA`, so if a handler adjusts a saved ERA in memory, the stub must write it back to the CSR.

## Testing & Bring-up Self-Checks

- Build test kernel and run in QEMU: `make -C rocinante test`
- Expected behavior: per-test PASS/FAIL lines over UART and then a stable summary line (`ALL TESTS PASSED` or `TESTS FAILED`) before halting.

Add tests for new features and bug fixes via `src/testing/tests.cpp`. More documentation TBD.

## Review Checklist (quick)

Before sending a change:

- Are all architecture-defined numbers named and explained?
- Are abbreviations expanded where they matter?
- Is the assembly sufficiently commented to be maintained by someone new?
- Are the flaws obvious and documented?
- Are ABI/layout assumptions asserted (or at least documented) where possible?
- Does the code remain friendly to freestanding/kernel constraints?


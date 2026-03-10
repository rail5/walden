# Rocinante

> [T]he first and foremost of all the hacks in the world.

&mdash; Miguel de Cervantes Saavedra, *Don Quixote*

> In many ways, Rozinante is not only Don Quixote's horse, but also his double; like Don Quixote, he is awkward, past his prime, and engaged in a task beyond his capacities.

&mdash; Wikipedia, [Rocinante](https://en.wikipedia.org/wiki/Rocinante)

Rocinante is the kernel of the Walden operating system. It is a microkernel written in C++23 (GNU dialect) and built with Clang/LLVM for the LoongArch64 architecture.

## Building and Running

**NOTE**: Upstream QEMU has some accuracy bugs in its LoongArch64 emulation. I have submitted patches to fix these, but before they're merged into the stable branch, you can build the [patched version of QEMU](https://github.com/gaosong715/qemu/tree/loongarch-for-upstream).

To build Rocinante, run `make` in this directory. The resulting kernel image will be located at `bin/rocinante.elf`.

To run Rocinante in QEMU, use `make run` for a graphical session or `make run-serial` for a serial console session.

You can run the tests with `make test`. This will build a *test version* of the kernel (i.e., a version that does not fully boot, but just runs the tests) and run it in QEMU via `run-serial`. The test output will be printed to the console.

### Build Requirements

TBD. The dev environment uses Clang/LLVM 21 on Debian. Earlier versions might be fine but I haven't checked.

## Editor / clangd

This repo is set up so clangd/clang-tidy can resolve repo-local includes.

- `compile_flags.txt` in the repo root provides a stable fallback compile command rooted at the workspace.
- For the best results (accurate per-file flags/defines), generate a compilation database:
	- From the repo root: `make compdb` (requires `bear`).

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

// This file provides a *minimal* subset of the C++ ABI/runtime hooks that
// g++ may reference.
//
// Why do we need this?
// --------------------
//   CPUCFG& GetCPUCFG() {
//     static CPUCFG instance;   // <-- compiler emits guard calls
//     return instance;
//   }
//
// Function-local statics use a "guard variable" so initialization happens only
// once. The compiler emits references to:
//   __cxa_guard_acquire
//   __cxa_guard_release
//   __cxa_guard_abort
//
// Additionally, C++ normally registers destructors of static objects so they
// run at program exit via:
//   __cxa_atexit
// and it uses:
//   __dso_handle
// to tag which binary/module owns those destructors.
//
// Our kernel will never call "exit" or unload itself, so we can
// safely stub out destructor registration.
//
// This file is intentionally tiny and readable. If/when we add SMP and need
// true thread-safe local-static initialization, we can upgrade the guard
// implementation to use atomics or a kernel spinlock.

#include <cstddef>
#include <cstdint>
#include <new>

#include "memory/heap.h"

extern "C" {

// Forward declarations:
// These symbols are part of the Itanium C++ ABI surface area that the compiler
// may reference. Declaring them before defining them keeps Clang happy when
// warnings like -Wmissing-prototypes are enabled.
int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso_handle);
extern void* __dso_handle;

int __cxa_guard_acquire(std::uint64_t* guard);
void __cxa_guard_release(std::uint64_t* guard);
void __cxa_guard_abort(std::uint64_t* guard);

[[noreturn]] void __cxa_pure_virtual();

// ---------------------------------------------------------------------------
// __cxa_atexit / __dso_handle
// ---------------------------------------------------------------------------
//
// What the toolchain expects:
// - __cxa_atexit registers a (destructor, object) pair to be called later.
// - __dso_handle is a unique address identifying this image/DSO.
//
// What we do in the kernel:
// - We never "exit" the kernel.
// - We never unload the kernel.
// Therefore, there's nowhere meaningful to run these destructors.
//
// Returning 0 indicates "registration succeeded".
int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso_handle) {
	(void)destructor;
	(void)arg;
	(void)dso_handle;
	return 0;
}

// The runtime expects this symbol to exist if __cxa_atexit is referenced.
// Any unique address is fine; using the variable address itself is apparently typical.
void* __dso_handle = &__dso_handle;

// ---------------------------------------------------------------------------
// Guard variables for function-local statics
// ---------------------------------------------------------------------------
//
// What the compiler expects:
// - A guard object is 64 bits (or larger) and the ABI uses the *first byte*
//   as an "initialized" flag.
// - __cxa_guard_acquire returns nonzero if the caller should run the
//   initialization.
// - __cxa_guard_release marks the object initialized.
// - __cxa_guard_abort is called if initialization throws.
//
// Our constraints:
// - Exceptions are disabled (-fno-exceptions), so abort isn't expected to be
//   called.
// - Early boot is effectively single-threaded.
//
// This implementation is NOT SMP-safe. If multiple cores can call into code
// that contains function-local statics concurrently, we must add locking.
int __cxa_guard_acquire(std::uint64_t* guard) {
	// Treat the first byte as "initialized". 0 => not initialized.
	auto* flag = reinterpret_cast<volatile std::uint8_t*>(guard);
	return (*flag == 0) ? 1 : 0;
}

void __cxa_guard_release(std::uint64_t* guard) {
	auto* flag = reinterpret_cast<volatile std::uint8_t*>(guard);
	*flag = 1;
}

void __cxa_guard_abort(std::uint64_t* guard) {
	// If init fails, the ABI would normally reset the guard so init may be
	// retried. With exceptions disabled, we just leave it uninitialized.
	(void)guard;
}

// ---------------------------------------------------------------------------
// Pure virtual call handler
// ---------------------------------------------------------------------------
//
// If a pure virtual method is called (usually a bug), the compiler may emit a
// call to __cxa_pure_virtual. We'll just spin.
void __cxa_pure_virtual() {
	for (;;) {
		asm volatile("" ::: "memory");
	}
}

} // extern "C"

// ---------------------------------------------------------------------------
// Global new/delete for a freestanding kernel
// ---------------------------------------------------------------------------
//
// Why do we need this?
// - In hosted C++ (normal userspace), the C++ runtime + libc provide operator
//   new/delete (usually backed by malloc/free).
// - In our kernel we link with -nostdlib, so nobody provides those symbols.
//
// What behavior do we want?
// - operator new(size) must either return a valid pointer or (in standard C++)
//   throw std::bad_alloc.
// - But we compile with -fno-exceptions, so throwing is not viable.
//
// Therefore:
// - The normal throwing forms will SPIN on OOM.
// - The nothrow forms will return nullptr on OOM.
//
// This keeps failure modes explicit and avoids silently proceeding with null
// pointers when using plain `new`.
//
// This should probably be scrapped
// It's putting the cart before the horse a bit to worry about dynamic allocation before we have a memory manager,
// but it makes it easier to write C++ code that uses dynamic allocation early on without having to worry about the
// details of the heap implementation right away.
// At any rate, we'll rework this later.

namespace {

[[noreturn]] void OutOfMemorySpin() {
	for (;;) {
		asm volatile("" ::: "memory");
	}
}

} // namespace

// ---- operator new (single object) ----
void* operator new(std::size_t size) {
	if (void* p = Rocinante::Heap::Alloc(size)) return p;
	OutOfMemorySpin();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
	return Rocinante::Heap::Alloc(size);
}

// ---- operator new (array) ----
void* operator new[](std::size_t size) {
	if (void* p = Rocinante::Heap::Alloc(size)) return p;
	OutOfMemorySpin();
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
	return Rocinante::Heap::Alloc(size);
}

// ---- aligned operator new (over-aligned types) ----
void* operator new(std::size_t size, std::align_val_t align) {
	const std::size_t alignment = static_cast<std::size_t>(align);
	if (void* p = Rocinante::Heap::Alloc(size, alignment)) return p;
	OutOfMemorySpin();
}

void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
	return Rocinante::Heap::Alloc(size, static_cast<std::size_t>(align));
}

void* operator new[](std::size_t size, std::align_val_t align) {
	const std::size_t alignment = static_cast<std::size_t>(align);
	if (void* p = Rocinante::Heap::Alloc(size, alignment)) return p;
	OutOfMemorySpin();
}

void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
	return Rocinante::Heap::Alloc(size, static_cast<std::size_t>(align));
}

// ---- operator delete ----
void operator delete(void* ptr) noexcept {
	Rocinante::Heap::Free(ptr);
}

void operator delete[](void* ptr) noexcept {
	Rocinante::Heap::Free(ptr);
}

// Sized delete (the compiler may emit calls to these in C++14+).
void operator delete(void* ptr, std::size_t) noexcept {
	Rocinante::Heap::Free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
	Rocinante::Heap::Free(ptr);
}

// Aligned delete.
void operator delete(void* ptr, std::align_val_t) noexcept {
	Rocinante::Heap::Free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
	Rocinante::Heap::Free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
	Rocinante::Heap::Free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
	Rocinante::Heap::Free(ptr);
}

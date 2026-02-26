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

#include <cstdint>

extern "C" {

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

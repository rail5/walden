/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante::Memory::Heap {

// This module provides a simple kernel heap (dynamic allocator)
//
// Design goals
// ------------
// - No dependency on libc / libstdc++.
// - Works with C++ global new/delete.
// - Readable, easy to debug.
// - Good enough for early kernel bring-up.
//
// Non-goals (for now)
// -------------------
// - SMP-safe allocation (no locks yet).
// - Per-CPU caches, slabs, etc.
// - Returning memory to the host/firmware.

// Initializes the heap over the given (already-mapped) memory region.
//
// The region must be writable RAM and remain valid for the lifetime of the
// kernel.
void Init(void* heap_start, std::size_t heap_size_bytes);

// Returns true once Init() has been called.
bool IsInitialized();

// Allocates at least `size` bytes with `alignment` (power of two).
//
// Returns nullptr on failure.
void* Alloc(std::size_t size, std::size_t alignment = 16);

// Frees a pointer returned by Alloc().
void Free(void* ptr);

// Debug helpers
std::size_t TotalBytes();
std::size_t FreeBytes();

} // namespace Rocinante::Memory::Heap

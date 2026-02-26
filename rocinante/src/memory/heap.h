/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rocinante::Heap {

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

// Initializes the heap over the given memory region.
//
// The region must be writable RAM and remain valid for the lifetime of the
// kernel.
void Init(void* heap_start, std::size_t heap_size_bytes);

// Initializes a default heap backed by a static buffer in .bss.
//
// This is convenient early on when we don't yet have a physical memory map.
void InitDefault();

// Returns true once Init() has been called (either explicitly or via InitDefault()).
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

} // namespace Rocinante::Heap

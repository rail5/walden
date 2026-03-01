/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "memory.h"

#include "heap.h"

#include <src/sp/cpucfg.h>

extern "C" char _end;

namespace Rocinante::Memory {

namespace {

static bool g_inited = false;
static AddressLimits g_limits{};
static std::uintptr_t g_recommended_heap_base = 0;

static constexpr std::uintptr_t AlignUp(std::uintptr_t value, std::size_t alignment) {
	// Align `value` up to the next multiple of `alignment`.
	//
	// NOTE: For current uses (heap base alignment) `alignment` is a small constant
	// (e.g. 16). We implement the general form here to avoid relying on the
	// power-of-two bitmask trick.
	if (alignment == 0) return value;
	const std::uintptr_t remainder = value % alignment;
	if (remainder == 0) return value;
	return value + (alignment - remainder);
}

static std::uint64_t MaxForWidth(std::uint32_t width_bits) {
	// This code path assumes CPUCFG-reported address widths are within the range
	// our masks can represent.
	// If a future CPU reports 64, "2^64 - 1" would overflow; treat it as all-ones.
	if (width_bits >= 64) return ~0ull;
	return (1ull << width_bits) - 1ull;
}

} // namespace

void InitEarly() {
	if (g_inited) return;

	// 1) Ensure we have *some* dynamic allocation.
	//
	// This is a bootstrap heap backed by a static buffer in .bss. It's suitable
	// for early initialization when we do not yet have:
	// - a physical memory allocator (PMM)
	// - a page allocator
	// - paging enabled / a VMM
	Rocinante::Heap::InitDefault();

	// 2) Snapshot CPU-reported address width limits.
	//
	// This informs later memory-layout choices:
	// - which virtual addresses are valid to use at all (VALEN)
	// - how wide physical addresses can be (PALEN)
	//
	// Note: this doesn't *by itself* allocate or map anything.
	auto& cpucfg = Rocinante::GetCPUCFG();
	g_limits.VALEN = cpucfg.VirtualAddressBits();
	g_limits.PALEN = cpucfg.PhysicalAddressBits();
	g_limits.VirtualMax = MaxForWidth(g_limits.VALEN);
	g_limits.PhysicalMax = MaxForWidth(g_limits.PALEN);

	// 3) Recommend a future heap placement.
	//
	// We can *recommend* "heap starts at end of kernel" now, but we cannot
	// actually use it until there are page tables mapping that region.
	g_recommended_heap_base = AlignUp(reinterpret_cast<std::uintptr_t>(&_end), 16);

	g_inited = true;
}

const AddressLimits& Limits() {
	// If someone asks before InitEarly, do the safe thing.
	if (!g_inited) InitEarly();
	return g_limits;
}

std::uintptr_t RecommendedHeapVirtualBase() {
	if (!g_inited) InitEarly();
	return g_recommended_heap_base;
}

void InitHeapAfterPaging(void* heap_base, std::size_t heap_size_bytes) {
	// This is the hand-off point: once our paging/VMM maps a dedicated heap
	// region, re-point the allocator at it.
	Rocinante::Heap::Init(heap_base, heap_size_bytes);
}

} // namespace Rocinante::Memory

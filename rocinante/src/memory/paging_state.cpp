/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "paging_state.h"

#include <src/platform/power.h>

namespace Rocinante::Memory {

namespace {

static bool g_paging_state_initialized = false;
static Rocinante::Memory::PagingState g_paging_state{};

} // namespace

void InitializePagingState(Rocinante::Memory::PagingState paging_state) {
	if (g_paging_state_initialized) {
		// This should only happen if boot code tries to "reinitialize" paging.
		Rocinante::Platform::Halt();
	}

	g_paging_state = paging_state;
	g_paging_state_initialized = true;
}

const Rocinante::Memory::PagingState* TryGetPagingState() {
	return g_paging_state_initialized ? &g_paging_state : nullptr;
}

} // namespace Rocinante::Memory

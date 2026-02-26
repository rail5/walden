/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "kernel.h"

#include <src/sp/cpucfg.h>

extern "C" [[noreturn]] void kernel_main(uint64_t a0, uint64_t a1, uint64_t a2) {
	auto& cpucfg = Rocinante::GetCPUCFG();

	// Just idle for now
	while (true) {
		asm volatile("" ::: "memory");
	}
}

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

extern "C" {
	[[noreturn]] void kernel_main(uint64_t a0, uint64_t a1, uint64_t a2);
}

/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

namespace Rocinante {
class Uart16550;
} // namespace Rocinante

namespace Rocinante::Memory {
class PhysicalMemoryManager;
} // namespace Rocinante::Memory

namespace Rocinante::Kernel {

void RunPagingBringup(
	const Rocinante::Uart16550& uart,
	Rocinante::Memory::PhysicalMemoryManager* pmm,
	std::uintptr_t kernel_physical_base,
	std::uintptr_t kernel_physical_end,
	void (*post_paging_continuation)()
);

} // namespace Rocinante::Kernel

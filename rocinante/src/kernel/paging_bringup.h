/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

namespace Rocinante {
class Uart16550;
}

namespace Rocinante::Memory {
class PhysicalMemoryManager;
}

namespace Rocinante::Kernel {

void RunPagingBringup(
	Rocinante::Uart16550& uart,
	Rocinante::Memory::PhysicalMemoryManager& pmm,
	std::uintptr_t kernel_physical_base,
	std::uintptr_t kernel_physical_end
);

} // namespace Rocinante::Kernel

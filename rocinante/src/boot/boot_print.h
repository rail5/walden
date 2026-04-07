/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <src/memory/boot_memory_map.h>

namespace Rocinante {
class Uart16550;
} // namespace Rocinante

namespace Rocinante::Memory {
class PhysicalMemoryManager;
} // namespace Rocinante::Memory

namespace Rocinante::Boot {

void PrintBootMemoryMap(const Rocinante::Uart16550& uart, const Rocinante::Memory::BootMemoryMap& map);
void PrintPhysicalMemoryManagerSummary(const Rocinante::Uart16550& uart, const Rocinante::Memory::PhysicalMemoryManager& pmm);

} // namespace Rocinante::Boot

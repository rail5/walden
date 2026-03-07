/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

namespace Rocinante::Platform {

[[noreturn]] void Halt();
[[noreturn]] void Shutdown();

// Bring-up hook: allow paging bring-up to retarget the syscon MMIO base to a
// mapped higher-half alias once paging is enabled.
void SetSysconBaseAddress(std::uintptr_t base_address);

} // namespace Rocinante::Platform

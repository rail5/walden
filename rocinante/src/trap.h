/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

// Compatibility header.
//
// The trap subsystem has been reorganized into src/trap/.
// Keep this file as the stable include path for now so call sites (including
// tests) remain readable during refactors.
#include <src/trap/trap.h>

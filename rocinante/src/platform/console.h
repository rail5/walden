/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

namespace Rocinante {
class Uart16550;
} // namespace Rocinante

namespace Rocinante::Platform {

// Early console used during bring-up. This is intentionally a simple global
// resource (not dynamically allocated) so that trap handlers and early boot
// paths can always print diagnostic information.
Rocinante::Uart16550& GetEarlyUart();

} // namespace Rocinante::Platform

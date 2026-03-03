/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/platform/console.h>

#include <src/platform/qemu_virt.h>
#include <src/sp/uart16550.h>

namespace {

static constinit Rocinante::Uart16550 g_uart(Rocinante::Platform::QemuVirt::kUartBase);

} // namespace

namespace Rocinante::Platform {

Rocinante::Uart16550& GetEarlyUart() {
	return g_uart;
}

} // namespace Rocinante::Platform

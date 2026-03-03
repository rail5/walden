/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/sp/cpucfg.h>

namespace Rocinante {

CPUCFG& GetCPUCFG() {
	static CPUCFG instance;
	return instance;
}

} // namespace Rocinante

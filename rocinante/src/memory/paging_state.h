/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include <src/memory/paging.h>

namespace Rocinante::Memory {

/**
 * @brief Persistent paging state established during boot.
 *
 * Purpose:
 * - Provide a single authoritative place to query the active paging root and
 *   CPU-reported address width configuration after paging is enabled.
 *
 * Policy:
 * - This is intentionally small. It is not yet a full virtual memory manager
 *   interface.
 *
 * Invariants:
 * - The state is populated exactly once during boot.
 * - The `root.root_physical_address` is a physical address.
 */
struct PagingState final {
	Rocinante::Memory::Paging::PageTableRoot root;
	Rocinante::Memory::Paging::AddressSpaceBits address_bits;
};

/**
 * @brief Initializes the global paging state.
 *
 * Ordering:
 * - Call this during paging initialization once a root page table exists.
 *
 * Safety:
 * - Overwriting an already-initialized state is treated as a programming error.
 */
void InitializePagingState(Rocinante::Memory::PagingState paging_state);

/**
 * @brief Returns the paging state if it has been initialized.
 */
const Rocinante::Memory::PagingState* TryGetPagingState();

} // namespace Rocinante::Memory

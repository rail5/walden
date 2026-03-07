/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <src/helpers/optional.h>

namespace Rocinante::Memory {

// Minimal kernel virtual-address allocator (bring-up).
//
// This is intentionally a simple bump allocator over a fixed range. It exists
// to stop hard-coding ad-hoc virtual placement policy in paging bring-up.
//
// Properties:
// - Not thread-safe.
// - Does not support free.
// - Caller defines the address-space range [base, limit).
class KernelVirtualAddressAllocator final {
	static constexpr std::uintptr_t AlignUp(std::uintptr_t value, std::size_t alignment) {
		if (alignment == 0) return value;
		const std::uintptr_t remainder = value % alignment;
		if (remainder == 0) return value;
		return value + (alignment - remainder);
	}

	std::uintptr_t m_cursor = 0;
	std::uintptr_t m_limit = 0;

	public:
		KernelVirtualAddressAllocator() = default;

		void Init(std::uintptr_t base, std::uintptr_t limit) {
			m_cursor = base;
			m_limit = limit;
		}

		bool IsInitialized() const {
			return m_limit != 0 && m_cursor != 0 && m_cursor <= m_limit;
		}

		std::uintptr_t Cursor() const { return m_cursor; }
		std::uintptr_t Limit() const { return m_limit; }

		Rocinante::Optional<std::uintptr_t> Allocate(std::size_t size_bytes, std::size_t alignment_bytes) {
			if (!IsInitialized()) return Rocinante::nullopt;
			if (size_bytes == 0) return Rocinante::nullopt;

			const std::uintptr_t base = AlignUp(m_cursor, alignment_bytes);
			const std::uintptr_t end = base + static_cast<std::uintptr_t>(size_bytes);
			if (end < base) return Rocinante::nullopt;
			if (end > m_limit) return Rocinante::nullopt;

			m_cursor = end;
			return base;
		}
};

} // namespace Rocinante::Memory

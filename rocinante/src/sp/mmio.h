/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */


#pragma once

#include <cstdint>
#include <type_traits>

namespace Rocinante {

// 'Concept' to declare that a given number is a valid bit width
// Acceptable widths: 8, 16, 32, 64
template<std::uint32_t W>
concept ValidBitWidth = (W == 8) || (W == 16) || (W == 32) || (W == 64);

template <std::uint32_t W>
requires ValidBitWidth<W>
struct MMIO final {
	// Constexpr-determine which integer type to use based on the specified bit width W
	using ValueType =
		std::conditional_t<W <= 8, std::uint8_t,
		  std::conditional_t<W <= 16, std::uint16_t,
		    std::conditional_t<W <= 32, std::uint32_t,
		      std::uint64_t>>>;
	
	static inline void write(std::uintptr_t address, ValueType value) {
		*reinterpret_cast<volatile ValueType*>(address) = value;
	}

	static inline ValueType read(std::uintptr_t address) {
		return *reinterpret_cast<volatile ValueType*>(address);
	}
};

} // namespace Rocinante

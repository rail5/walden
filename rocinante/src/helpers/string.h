/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <type_traits>

#include <src/memory/heap.h>

namespace Rocinante {

/**
 * @brief Simple string implementation for the kernel to use without pulling in the C++ standard library.
 *
 * Should probably be thrown away altogether or reworked later.
 *
 * Kind of putting the cart before the horse with heap allocations before we have a memory manager.
 * 
 */
class String final {
	private:
		char* m_data;
		std::uint32_t m_capacity;
		std::uint32_t m_length;

		static constexpr std::uint32_t kInitialCapacityBytes = 16;

		void _initialize() {
			// The string invariant is: when m_data != nullptr, m_data[m_length] is always a NUL terminator.
			m_capacity = kInitialCapacityBytes;
			m_length = 0;
			m_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
			m_data[0] = '\0';
		}

		void _double_capacity() {
			m_capacity *= 2;
			char* new_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
			for (std::uint32_t i = 0; i < m_length; i++) {
				new_data[i] = m_data[i];
			}
			new_data[m_length] = '\0';
			Rocinante::Heap::Free(m_data);
			m_data = new_data;
		}
	public:
		String() : m_data(nullptr), m_capacity(0), m_length(0) {}
		String(const char* data, std::uint32_t length) : m_data(nullptr), m_capacity(0), m_length(0) {
			if (data == nullptr || length == 0) return;
			m_capacity = length + 1;
			m_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
			for (std::uint32_t i = 0; i < length; i++) {
				m_data[i] = data[i];
			}
			m_length = length;
			m_data[m_length] = '\0';
		}

		// Backward-compatible overload: treats `data` as input bytes and copies them.
		String(char* data, std::uint32_t length) : String(static_cast<const char*>(data), length) {}

		String(const String& other) : m_data(nullptr), m_capacity(0), m_length(0) {
			if (other.m_data == nullptr || other.m_length == 0) return;
			m_capacity = other.m_length + 1;
			m_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
			for (std::uint32_t i = 0; i < other.m_length; i++) {
				m_data[i] = other.m_data[i];
			}
			m_length = other.m_length;
			m_data[m_length] = '\0';
		}

		String& operator=(const String& other) {
			if (this == &other) return *this;

			if (m_data) {
				Rocinante::Heap::Free(m_data);
				m_data = nullptr;
				m_capacity = 0;
				m_length = 0;
			}

			if (other.m_data == nullptr || other.m_length == 0) return *this;

			m_capacity = other.m_length + 1;
			m_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
			for (std::uint32_t i = 0; i < other.m_length; i++) {
				m_data[i] = other.m_data[i];
			}
			m_length = other.m_length;
			m_data[m_length] = '\0';
			return *this;
		}

		String(String&& other) noexcept : m_data(other.m_data), m_capacity(other.m_capacity), m_length(other.m_length) {
			other.m_data = nullptr;
			other.m_capacity = 0;
			other.m_length = 0;
		}

		String& operator=(String&& other) noexcept {
			if (this == &other) return *this;
			if (m_data) Rocinante::Heap::Free(m_data);

			m_data = other.m_data;
			m_capacity = other.m_capacity;
			m_length = other.m_length;

			other.m_data = nullptr;
			other.m_capacity = 0;
			other.m_length = 0;
			return *this;
		}

		~String() {
			if (m_data) Rocinante::Heap::Free(m_data);
		}

		const char* data() const { return m_data; }
		std::uint32_t length() const { return m_length; }
		std::uint32_t size() const { return m_length; }

		void append(char c) {
			if (m_data == nullptr) _initialize();
			// Need room for the new character plus the trailing NUL terminator.
			while ((m_length + 2) > m_capacity) _double_capacity();
			m_data[m_length] = c;
			m_length++;
			m_data[m_length] = '\0';
		}

		void append(const char* str) {
			while (*str) append(*str++);
		}

		char at(std::uint32_t index) const {
			if (index >= m_length) return '\0'; // Out of bounds, return null character
			return m_data[index];
		}

		char operator[](std::uint32_t index) const {
			return at(index);
		}

		void append(const String& other) {
			for (std::uint32_t i = 0; i < other.length(); i++) {
				append(other.at(i));
			}
		}

		String& operator+=(const String& other) {
			append(other);
			return *this;
		}

		String operator+(const String& other) const {
			String result = *this;
			result.append(other);
			return result;
		}

		String operator+(const char* other) const {
			String result = *this;
			result.append(other);
			return result;
		}

		String& operator+=(const char* other) {
			append(other);
			return *this;
		}

		const char* c_str() const {
			if (m_data == nullptr) return "";
			return m_data;
		}
};

// Converting integer types to strings
template<typename T>
requires std::is_integral_v<T>
String to_string(T value) {
	String result;
	if (value == 0) {
		result.append('0');
		return result;
	}

	bool is_negative = false;
	using UnsignedT = std::make_unsigned_t<T>;
	UnsignedT magnitude = static_cast<UnsignedT>(value);
	if constexpr (std::is_signed_v<T>) {
		if (value < 0) {
			is_negative = true;
			// Avoid UB for INT_MIN by doing the negation in unsigned space.
			magnitude = static_cast<UnsignedT>(0) - static_cast<UnsignedT>(value);
		}
	}

	char buffer[20]; // Enough to hold the string representation of any 64-bit integer
	int index = 0;
	while (magnitude > 0) {
		buffer[index++] = static_cast<char>('0' + (magnitude % 10));
		magnitude /= 10;
	}

	if (is_negative) {
		result.append('-');
	}

	for (int i = index - 1; i >= 0; i--) {
		result.append(buffer[i]);
	}

	return result;
}

} // namespace Rocinante

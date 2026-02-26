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
		uint32_t m_capacity;
		uint32_t m_length;

		void _initialize() {
			m_capacity = 16; // Start with a small capacity and grow as needed
			m_length = 0;
			m_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
		}

		void _double_capacity() {
			m_capacity *= 2;
			char* new_data = static_cast<char*>(Rocinante::Heap::Alloc(m_capacity, alignof(char)));
			for (uint32_t i = 0; i < m_length; i++) {
				new_data[i] = m_data[i];
			}
			Rocinante::Heap::Free(m_data);
			m_data = new_data;
		}
	public:
		String() : m_data(nullptr), m_length(0) {}
		String(char* data, uint32_t length) : m_data(data), m_length(length) {}

		~String() {
			if (m_data) Rocinante::Heap::Free(m_data);
		}

		const char* data() const { return m_data; }
		uint32_t length() const { return m_length; }
		uint32_t size() const { return m_length; }

		void append(char c) {
			if (m_data == nullptr) _initialize();
			if (m_length + 1 >= m_capacity) _double_capacity();
			m_data[m_length] = c;
			m_length++;
		}

		void append(const char* str) {
			while (*str) append(*str++);
		}

		char at(uint32_t index) const {
			if (index >= m_length) return '\0'; // Out of bounds, return null character
			return m_data[index];
		}

		char operator[](uint32_t index) const {
			return at(index);
		}

		void append(const String& other) {
			for (uint32_t i = 0; i < other.length(); i++) {
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
	if constexpr (std::is_signed_v<T>) {
		if (value < 0) {
			is_negative = true;
			value = -value; // This is safe even for the most negative value of a signed type due to two's complement representation
		}
	}

	char buffer[20]; // Enough to hold the string representation of any 64-bit integer
	int index = 0;
	while (value > 0) {
		buffer[index++] = '0' + (value % 10);
		value /= 10;
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

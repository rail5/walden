/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <new>
#include <utility>

namespace Rocinante {

struct NullOpt {
	struct Init {};
	constexpr explicit NullOpt(Init) {}
};
inline constexpr NullOpt nullopt{NullOpt::Init{}};

/**
 * @brief A simple Optional<T> implementation for the kernel to use without pulling in the C++ standard library.
 * This is not intended to be a full implementation of std::optional, but rather a minimal implementation that
 * provides the basic functionality needed for the kernel.
 * 
 * @tparam T The type of the value that may or may not be present.
 */
template<typename T>
class Optional final {
	private:
		bool m_has_value{false};
		alignas(T) unsigned char m_value_storage[sizeof(T)];
		T* ptr() { return reinterpret_cast<T*>(m_value_storage); }
		const T* ptr() const { return reinterpret_cast<const T*>(m_value_storage); }

	public:
		Optional() = default;

		Optional(const T& value) { emplace(value); }
		Optional(T&& value) { emplace(std::move(value)); }

		Optional(const Optional& other) { if (other.m_has_value) emplace(*other.ptr()); }

		Optional& operator=(const Optional& other) {
			if (this == &other) return *this;

			if (m_has_value && other.m_has_value) {
				*ptr() = *other.ptr();
			} else if (m_has_value && !other.m_has_value) {
				reset();
			} else if (!m_has_value && other.m_has_value) {
				emplace(*other.ptr());
			}
			return *this;
		}

		Optional(NullOpt) noexcept : m_has_value(false) {}
		Optional& operator=(NullOpt) noexcept { reset(); return *this; }

		~Optional() { reset(); }

		void reset() {
			if (m_has_value) {
				ptr()->~T();
				m_has_value = false;
			}
		}

		template<typename... Args>
		T& emplace(Args&&... args) {
			reset();
			::new (static_cast<void*>(m_value_storage)) T(std::forward<Args>(args)...);
			m_has_value = true;
			return *ptr();
		}


		bool has_value() const { return m_has_value; }
		T& value() { return *ptr(); }
		const T& value() const { return *ptr(); }
		const T& value_or(const T& default_value) const { return m_has_value ? *ptr() : default_value; }

		const T& operator*() const { return *ptr(); }
		T& operator*() { return *ptr(); }
		const T* operator->() const { return ptr(); }
		T* operator->() { return ptr(); }

		explicit operator bool() const noexcept { return m_has_value; }
};


} // namespace Rocinante

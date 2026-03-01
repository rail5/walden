/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "uart16550.h"
#include "mmio.h"

void Rocinante::Uart16550::putc(char c) const {
	if (c == '\n') putc('\r'); // Prepend a carriage return for newlines to ensure proper formatting on terminals that expect it

	while ((MMIO<8>::read(m_base_address + OFFSET_LINE_STATUS) & LINE_STATUS_THR_EMPTY) == 0) {
		// Wait for the transmitter holding register to be empty before writing the next byte
	}

	MMIO<8>::write(m_base_address + OFFSET_TRANSMIT_HOLDING, static_cast<std::uint8_t>(c));
}

void Rocinante::Uart16550::puts(const char* str) const {
	while (*str) putc(*str++);
}

std::uint8_t Rocinante::Uart16550::read_iir() const {
	return MMIO<8>::read(m_base_address + OFFSET_INTERRUPT_IDENTIFICATION);
}

void Rocinante::Uart16550::enable_rx_irq() const {
	MMIO<8>::write(m_base_address + OFFSET_INTERRUPT_ENABLE, INTERRUPT_ENABLE_RECEIVED_DATA_AVAILABLE);
}

bool Rocinante::Uart16550::rx_ready() const {
	return (MMIO<8>::read(m_base_address + OFFSET_LINE_STATUS) & LINE_STATUS_DATA_READY) != 0;
}

void Rocinante::Uart16550::irq_rx_drain() const {
	// Drain any pending RX bytes and enqueue them
	// This is safe to call from an IRQ context
	while (rx_ready()) {
		const std::uint8_t byte = MMIO<8>::read(m_base_address + OFFSET_RECEIVER_BUFFER);
		const std::uint32_t head = m_receive_buffer_head;
		const std::uint32_t next = (head + 1U) & RECEIVE_BUFFER_MASK;
		if (next != m_receive_buffer_tail) { // Check for buffer overflow; if the buffer is full, we just drop the byte on the floor
			m_receive_buffer[head] = byte;
			m_receive_buffer_head = next;
		}
	}
}

bool Rocinante::Uart16550::irq_try_getc(char* out) const {
	const std::uint32_t tail = m_receive_buffer_tail;
	if (tail == m_receive_buffer_head) return false; // Buffer is empty

	*out = static_cast<char>(m_receive_buffer[tail]);
	m_receive_buffer_tail = (tail + 1U) & RECEIVE_BUFFER_MASK;
	return true;
}

char Rocinante::Uart16550::getc() const {
	for (;;) {
		char c;
		if (irq_try_getc(&c)) return c;
		// Sleep until *some* interrupt occurs
		// TODO(@rail5): This is bad design. This function wakes for every interrupt, even if it's not a UART interrupt.
		// We should ideally have a way to sleep until a specific interrupt or event occurs.
		// Should just register a proper interrupt handler.
		asm volatile("idle 0" ::: "memory");
	}
}

void Rocinante::Uart16550::write_hex_u64(std::uint64_t value) const {
	// Fixed-width hex formatting: 0x + 16 nybbles.
	// This is intentionally allocation-free for trap safety.
	static constexpr char kHexDigits[] = "0123456789abcdef";
	static constexpr int kBitsPerNybble = 4;
	static constexpr int kNybblesInU64 = 16;
	static constexpr int kTopNybbleShift = (kNybblesInU64 - 1) * kBitsPerNybble;

	puts("0x");
	for (int shift = kTopNybbleShift; shift >= 0; shift -= kBitsPerNybble) {
		const std::uint8_t nybble = static_cast<std::uint8_t>((value >> shift) & 0xFu);
		putc(kHexDigits[nybble]);
	}
}

void Rocinante::Uart16550::write_dec_u64(std::uint64_t value) const {
	// Minimal unsigned decimal formatting.
	// Max digits in u64 is 20.
	char buffer[21];
	int pos = 0;

	if (value == 0) {
		putc('0');
		return;
	}

	while (value != 0 && pos < static_cast<int>(sizeof(buffer))) {
		buffer[pos++] = static_cast<char>('0' + (value % 10));
		value /= 10;
	}

	for (int i = pos - 1; i >= 0; i--) {
		putc(buffer[i]);
	}
}

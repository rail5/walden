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

	MMIO<8>::write(m_base_address + OFFSET_TRANSMIT_HOLDING, static_cast<uint8_t>(c));
}

void Rocinante::Uart16550::puts(const char* str) const {
	while (*str) putc(*str++);
}

uint8_t Rocinante::Uart16550::read_iir() const {
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
		const uint8_t byte = MMIO<8>::read(m_base_address + OFFSET_RECEIVER_BUFFER);
		const uint32_t head = m_receive_buffer_head;
		const uint32_t next = (head + 1U) & RECEIVE_BUFFER_MASK;
		if (next != m_receive_buffer_tail) { // Check for buffer overflow; if the buffer is full, we just drop the byte on the floor
			m_receive_buffer[head] = byte;
			m_receive_buffer_head = next;
		}
	}
}

bool Rocinante::Uart16550::irq_try_getc(char* out) const {
	const uint32_t tail = m_receive_buffer_tail;
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

void Rocinante::Uart16550::write(Rocinante::String str) const {
	puts(str.c_str());
}

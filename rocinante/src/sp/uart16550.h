/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include <src/helpers/string.h>

namespace Rocinante {

class Uart16550 final {
	private:
		uintptr_t m_base_address;

		// Offsets of the 16550 registers from the base address
		static constexpr uintptr_t OFFSET_RECEIVER_BUFFER = 0x00; // Read-only, used to read received data
		static constexpr uintptr_t OFFSET_TRANSMIT_HOLDING = 0x00; // Write a byte here to transmit it
		static constexpr uintptr_t OFFSET_INTERRUPT_ENABLE = 0x01; // Interrupt enable bits
		static constexpr uintptr_t OFFSET_INTERRUPT_IDENTIFICATION = 0x02; // Read-only, tells you which interrupt(s) are pending
		static constexpr uintptr_t OFFSET_LINE_STATUS = 0x5; // Read-only, contains bits indicating if the transmitter is empty, if data is available to read, etc.

		// Line status flags
		static constexpr uint8_t LINE_STATUS_DATA_READY = 0x01; // Set if there is data available to read
		static constexpr uint8_t LINE_STATUS_THR_EMPTY = 0x20; // Set if the transmitter holding register is empty and ready for a new byte to transmit

		// Interrupt enable flag
		static constexpr uint8_t INTERRUPT_ENABLE_RECEIVED_DATA_AVAILABLE = 0x01; // If set, an interrupt will be triggered when data is available to read

		// Receive ring buffer for incoming data
		static constexpr uint32_t RECEIVE_BUFFER_SIZE = 1024;
		static constexpr uint32_t RECEIVE_BUFFER_MASK = RECEIVE_BUFFER_SIZE - 1;
		static_assert((RECEIVE_BUFFER_SIZE & RECEIVE_BUFFER_MASK) == 0, "Receive buffer size must be a power of 2");

		mutable volatile uint32_t m_receive_buffer_head = 0;
		mutable volatile uint32_t m_receive_buffer_tail = 0;
		mutable uint8_t m_receive_buffer[RECEIVE_BUFFER_SIZE]{};
	
	public:
		constexpr explicit Uart16550(uintptr_t base_address) : m_base_address(base_address) {}

		Uart16550(const Uart16550&) = delete;
		Uart16550& operator=(const Uart16550&) = delete;

		void putc(char c) const;
		void puts(const char* str) const;

		enum class IrqCause : uint8_t {
			None,
			ModemStatus,
			TransmitterHoldingRegisterEmpty,
			ReceivedDataAvailable,
			ReceiverLineStatus, // This value is returned when both bits 2 and 3 are set, which indicates a receiver line status interrupt
			CharacterTimeout, // This value is returned when both bits 2 and 3 are set, and there is no data available to read.
				// This indicates a "character timeout" interrupt, which means the UART has been sitting with received data available
				// to read for a while without the CPU reading it, which may indicate a problem with the CPU not servicing the UART
				// interrupts in a timely manner.
			Unknown
		};

		uint8_t read_iir() const;
		static constexpr IrqCause decode_iir(uint8_t iir) {
			// Bit 0 of the IIR is set to 0 if an interrupt is pending, and 1 if no interrupts are pending.
			// So if bit 0 is 1, we can immediately return IrqCause::None without looking at the other bits.
			if ((iir & 0x01) != 0) return IrqCause::None;

			// Bits 3:1 of the IIR indicate the highest priority pending interrupt, if any.
			switch ((iir >> 1) & 0x07) {
				case 0x0: return IrqCause::ModemStatus;
				case 0x1: return IrqCause::TransmitterHoldingRegisterEmpty;
				case 0x2: return IrqCause::ReceivedDataAvailable;
				case 0x3: return IrqCause::ReceiverLineStatus;
				case 0x6: return IrqCause::CharacterTimeout;
				default: return IrqCause::Unknown;
			}
		}

		IrqCause irq_cause() const { return decode_iir(read_iir()); }

		// Enables the "Received Data Available" interrupt, which will cause the UART to trigger an interrupt
		// whenever data is available to read. The CPU's interrupt handler can then call the receive() method
		// to read the data from the UART and store it in the receive buffer.
		void enable_rx_irq() const;

		bool rx_ready() const;

		// Drains the receive FIFO and stores the received data in the receive buffer.
		// Should be called from the CPU's interrupt handler when a "Received Data Available" interrupt is triggered.
		void irq_rx_drain() const;
	
		// Tries to get a received character from the receive buffer.
		// Returns true and sets 'out' if a character was available, or returns false if the receive buffer was empty.
		bool irq_try_getc(char* out) const; 

		// Blocking read: waits until a character is available in the receive buffer, then returns it.
		char getc() const;

		void write(Rocinante::String str) const;
};

} // namespace Rocinante

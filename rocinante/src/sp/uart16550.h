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
		std::uintptr_t m_base_address;

		// Offsets of the 16550 registers from the base address
		static constexpr std::uintptr_t OFFSET_RECEIVER_BUFFER = 0x00; // Read-only, used to read received data
		static constexpr std::uintptr_t OFFSET_TRANSMIT_HOLDING = 0x00; // Write a byte here to transmit it
		static constexpr std::uintptr_t OFFSET_INTERRUPT_ENABLE = 0x01; // Interrupt enable bits
		static constexpr std::uintptr_t OFFSET_INTERRUPT_IDENTIFICATION = 0x02; // Read-only, tells you which interrupt(s) are pending
		static constexpr std::uintptr_t OFFSET_LINE_STATUS = 0x05; // Read-only, contains bits indicating if the transmitter is empty, if data is available to read, etc.

		// Line status flags
		static constexpr std::uint8_t LINE_STATUS_DATA_READY = 0x01; // Set if there is data available to read
		static constexpr std::uint8_t LINE_STATUS_THR_EMPTY = 0x20; // Set if the transmitter holding register is empty and ready for a new byte to transmit

		// Interrupt enable flag
		static constexpr std::uint8_t INTERRUPT_ENABLE_RECEIVED_DATA_AVAILABLE = 0x01; // If set, an interrupt will be triggered when data is available to read

		// Receive ring buffer for incoming data
		static constexpr std::uint32_t RECEIVE_BUFFER_SIZE = 1024;
		static constexpr std::uint32_t RECEIVE_BUFFER_MASK = RECEIVE_BUFFER_SIZE - 1;
		static_assert((RECEIVE_BUFFER_SIZE & RECEIVE_BUFFER_MASK) == 0, "Receive buffer size must be a power of 2");

		mutable volatile std::uint32_t m_receive_buffer_head = 0;
		mutable volatile std::uint32_t m_receive_buffer_tail = 0;
		mutable std::uint8_t m_receive_buffer[RECEIVE_BUFFER_SIZE]{};
	
	public:
		constexpr explicit Uart16550(std::uintptr_t base_address) : m_base_address(base_address) {}

		Uart16550(const Uart16550&) = delete;
		Uart16550& operator=(const Uart16550&) = delete;

		void putc(char c) const;
		void puts(const char* str) const;

		enum class IrqCause : std::uint8_t {
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

		std::uint8_t read_iir() const;
		static constexpr IrqCause decode_iir(std::uint8_t interrupt_identification_register) {
			// Spec-defined constants from the 16550 UART Interrupt Identification Register (IIR).
			// IIR[0] == 1 means no interrupt pending; IIR[3:1] encodes the highest-priority pending IRQ.
			static constexpr std::uint8_t kIirNoInterruptPendingBit = 0x01;
			static constexpr std::uint8_t kIirCauseShift = 1;
			static constexpr std::uint8_t kIirCauseMask = 0x07;
			static constexpr std::uint8_t kIirCauseModemStatus = 0x0;
			static constexpr std::uint8_t kIirCauseTransmitterHoldingRegisterEmpty = 0x1;
			static constexpr std::uint8_t kIirCauseReceivedDataAvailable = 0x2;
			static constexpr std::uint8_t kIirCauseReceiverLineStatus = 0x3;
			static constexpr std::uint8_t kIirCauseCharacterTimeout = 0x6;

			if ((interrupt_identification_register & kIirNoInterruptPendingBit) != 0) return IrqCause::None;

			const std::uint8_t cause =
				(interrupt_identification_register >> kIirCauseShift) & kIirCauseMask;
			switch (cause) {
				case kIirCauseModemStatus: return IrqCause::ModemStatus;
				case kIirCauseTransmitterHoldingRegisterEmpty: return IrqCause::TransmitterHoldingRegisterEmpty;
				case kIirCauseReceivedDataAvailable: return IrqCause::ReceivedDataAvailable;
				case kIirCauseReceiverLineStatus: return IrqCause::ReceiverLineStatus;
				case kIirCauseCharacterTimeout: return IrqCause::CharacterTimeout;
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

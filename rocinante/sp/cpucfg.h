/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include <rocinante/helpers/optional.h>

namespace Rocinante {

/**
 * @brief The returned contents of the CPUCFG instruction as defined in the LoongArch64 v1.1 ISA specification.
 *
 * This class should not be instantiated by users.
 * Users should call GetCPUCFG() to get a reference to the single canonical instance of this class,
 * and call methods on that instance to query CPU configuration.
 *
 * Information is lazy-loaded and cached on first access, so users can call any method in any order
 * without worrying about performance implications of loading unused information.
 *
 * The CPUCFG instruction is defined to return implementation-defined information about the CPU configuration,
 * such as supported features and cache geometry. The exact meaning of the returned information is defined
 * in the LoongArch64 v1.1 ISA specification, and is documented in the comments for each method of this class.
 * 
 */
class CPUCFG final {
	public:
		enum class Architecture : uint8_t {
			SimplifiedLA32 = 0,
			LA32 = 1,
			LA64 = 2,
			Reserved = 3
		};

		struct CacheGeometry final {
			uint16_t WaysMinus1;
			uint8_t IndexLog2;
			uint8_t LineSizeLog2;

			constexpr uint32_t Ways() const { return static_cast<uint32_t>(WaysMinus1) + 1; }
			constexpr uint32_t SetsPerWay() const { return 1u << IndexLog2; }
			constexpr uint32_t LineSizeBytes() const { return 1u << LineSizeLog2; }
		};
	private:
		static constexpr bool _bit(uint32_t value, uint32_t bit_index) {
			return (value >> bit_index) & 0x1u;
		}

		static constexpr uint32_t _bits(uint32_t value, uint32_t high, uint32_t low) {
			const uint32_t width = (high - low) + 1;
			const uint64_t mask = (width >= 32) ? 0xFFFF'FFFFull : ((1ull << width) - 1ull);
			return static_cast<uint32_t>((static_cast<uint64_t>(value) >> low) & mask);
		}

		static uint32_t _read_word(uint32_t word_number) {
			uint32_t value;
			asm volatile(
				"cpucfg %0, %1"
				: "=r"(value)
				: "r"(word_number)
			);
			return value;
		}

		Rocinante::Optional<uint32_t>* _word_slot(uint32_t word_number) {
			switch (word_number) {
				case 0x0:
					return &m_word0;
				case 0x1:
					return &m_word1;
				case 0x2:
					return &m_word2;
				case 0x3:
					return &m_word3;
				case 0x4:
					return &m_word4;
				case 0x5:
					return &m_word5;
				case 0x6:
					return &m_word6;
				case 0x10:
					return &m_word10;
				case 0x11:
					return &m_word11;
				case 0x12:
					return &m_word12;
				case 0x13:
					return &m_word13;
				case 0x14:
					return &m_word14;
				default:
					return nullptr;
			}
		}

		uint32_t _load_word(uint32_t word_number) {
			const uint32_t value = _read_word(word_number);
			if (auto* slot = _word_slot(word_number)) {
				slot->emplace(value);
			}
			return value;
		}

		uint32_t _word(uint32_t word_number) {
			if (auto* slot = _word_slot(word_number)) {
				if (slot->has_value()) return slot->value();
			}
			return _load_word(word_number);
		}

		/**
		 * @brief Word number 0x0: Processor ID
		 *
		 * - Bits 31:0: PRID: Processor Identity
		 */
		Rocinante::Optional<uint32_t> m_word0;
		
		/**
		 * @brief Word number 0x1:
		 * - Bits 0-1: ARCH: 0 = simplifiedLA32, 1 = LA32, 2 = LA64, 3 = reserved
		 * - Bit 2: PGGMU: 1 = MMU supports page mapping mode, 0 = MMU does not support page mapping mode
		 * - Bit 3: IOCSR: 1 = CPU supports the IOCSR instruction, 0 = CPU does not support the IOCSR instruction
		 * - Bits 4-11: PALEN: "Supported physical address bits PALEN value minus 1"[sic]
		 *    TODO(@rail5): Does that mean this returns PALEN or PALEN-1? The wording is ambiguous
		 * - Bits 12-19: VALEN: "Supported virtual address bits VALEN value minus 1"[sic]
		 *    TODO(@rail5): Does that mean this returns VALEN or VALEN-1? The wording is ambiguous
		 * - Bit 20: UAL: 1 = CPU supports non-aligned memory access, 0 = CPU does not support non-aligned memory access
		 * - Bit 21: RI: 1 = support for page attribute "Read Inhibit", 0 = no support for page attribute "Read Inhibit"
		 * - Bit 22: EP: 1 = support for page attribute "Execution Protection", 0 = no support for page attribute "Execution Protection"
		 * - Bit 23: RPLV: 1 = support for page attribute RPLV, 0 = no support for page attribute RPLV
		 * - Bit 24: HP: 1 = support for "huge page", 0 = no support for "huge page"
		 * - Bit 25: CRC: 1 = support for CRC instruction (meaning: info such as 'Loongson3A5000 @ 2.5GHz'), 0 = no support for CRC instruction
		 * - Bit 26: MSGINT: 1 = external interrupt uses the "message interrupt" mode, 0 = external interrupt uses the "level interrupt line mode"
		 */
		Rocinante::Optional<uint32_t> m_word1;

		/**
		 * @brief Word number 0x2: ISA feature flags
		 * - Bit 0: FP: 1 indicates support for basic floating-point instructions
		 * - Bit 1: FP_SP: 1 indicates support for single-precision floating-point numbers
		 * - Bit 2: FP_DP: 1 indicates support for double-precision floating-point numbers
		 * - Bits 5:3: FP_ver: floating-point arithmetic standard version (1 = initial, IEEE 754-2008 compatible)
		 * - Bit 6: LSX: 1 indicates support for 128-bit vector extension
		 * - Bit 7: LASX: 1 indicates support for 256-bit vector extension
		 * - Bit 8: COMPLEX: 1 indicates support for complex vector operation instructions
		 * - Bit 9: CRYPTO: 1 indicates support for encryption/decryption vector instructions
		 * - Bit 10: LVZ: 1 indicates support for virtualization extension
		 * - Bits 13:11: LVZ_ver: virtualization HW acceleration version (1 = initial)
		 * - Bit 14: LLFTP: 1 indicates support for constant frequency counter and timer
		 * - Bits 17:15: LLFTP_ver: constant frequency counter and timer version (1 = initial)
		 * - Bit 18: LBT_X86: 1 indicates support for X86 binary translation extension
		 * - Bit 19: LBT_ARM: 1 indicates support for ARM binary translation extension
		 * - Bit 20: LBT_MIPS: 1 indicates support for MIPS binary translation extension
		 * - Bit 21: LSPW: 1 indicates support for the software page table walking instruction
		 * - Bit 22: LAM: 1 indicates support for AM* atomic memory access instruction
		 * - Bit 24: HPTW: 1 indicates support for Page Table Walker
		 * - Bit 25: FRECIPE: 1 indicates support for FRECIPE.{S/D}, FRSQRTE.{S/D} (and vector variants if LSX/LASX)
		 * - Bit 26: DIV32: 1 indicates DIV.W[U]/MOD.W[U] on LA64 only compute low 32-bit of inputs
		 * - Bit 27: LAM_BH: 1 indicates support for AM{SWAP/ADD}[_DB].{B/H}
		 * - Bit 28: LAMCAS: 1 indicates support for AMCAS[_DB].{B/H/W/D}
		 * - Bit 29: LLACQ_SCREL: 1 indicates support for LLACQ.{W/D}, SCREL.{W/D}
		 * - Bit 30: SCQ: 1 indicates support for SC.Q
		 */
		Rocinante::Optional<uint32_t> m_word2;

		/**
		 * @brief Word number 0x3: MMU/page-walk and memory ordering feature flags
		 * - Bit 0: CCDMA: 1 indicates support for hardware Cache-coherent DMA
		 * - Bit 1: SFB: 1 indicates support for Store Fill Buffer (SFB)
		 * - Bit 2: UCACC: 1 indicates support for UCACC window
		 * - Bit 3: LLEXC: 1 indicates support for LL instruction to fetch exclusive block function
		 * - Bit 4: SCDLY: 1 indicates support random delay function after SC
		 * - Bit 5: LLDBAR: 1 indicates support LL automatic with dbar function
		 * - Bit 6: ITLBTHMC: 1 indicates hardware maintains consistency between ITLB and TLB
		 * - Bit 7: ICHMC: 1 indicates hardware maintains data consistency between ICache and DCache in one core
		 * - Bits 10:8: SPW_LVL: maximum number of directory levels supported by page walk instruction
		 * - Bit 11: SPW_HP_HF: 1 indicates page walk fills TLB in half when it encounters a large page
		 * - Bit 12: RVA: 1 indicates software configuration can shorten virtual address range
		 * - Bits 16:13: RVAMAX-1: maximum configurable virtual address shortened by -1
		 * - Bit 17: DBAR_hints: 1 indicates non-0 DBAR values implemented per recommended manual meaning
		 * - Bit 23: LD_SEQ_SA: 1 indicates hardware guarantees sequential execution of loads at same address
		 */
		Rocinante::Optional<uint32_t> m_word3;

		/**
		 * @brief Word number 0x4: Constant frequency counter crystal frequency
		 * - Bits 31:0: CC_FREQ: constant frequency timer and the crystal frequency corresponding to the timer clock
		 */
		Rocinante::Optional<uint32_t> m_word4;

		/**
		 * @brief Word number 0x5: Constant frequency counter clock multiplication/division factors
		 * - Bits 15:0: CC_MUL: constant frequency timer clock multiplication factor
		 * - Bits 31:16: CC_DIV: constant frequency timer clock division coefficient
		 */
		Rocinante::Optional<uint32_t> m_word5;

		/**
		 * @brief Word number 0x6: Performance monitor configuration
		 * - Bit 0: PMP: 1 indicates support for performance counter
		 * - Bits 3:1: PMVER: performance monitor event version (1 = initial)
		 * - Bits 7:4: PMNUM: number of performance monitors minus 1
		 * - Bits 13:8: PMBITS: number of bits of a performance monitor minus 1
		 * - Bit 14: UPM: 1 indicates support for reading performance counter in user mode
		 */
		Rocinante::Optional<uint32_t> m_word6;

		/**
		 * @brief Word number 0x10: Cache presence/relationship flags
		 * - Bit 0: L1 IU_Present: 1 indicates L1 instruction cache or L1 unified cache is present
		 * - Bit 1: L1 IU Unify: 1 indicates the cache in L1 IU_Present is unified
		 * - Bit 2: L1 D Present: 1 indicates L1 data cache is present
		 * - Bit 3: L2 IU Present: 1 indicates L2 instruction cache or L2 unified cache is present
		 * - Bit 4: L2 IU Unify: 1 indicates the cache in L2 IU_Present is unified
		 * - Bit 5: L2 IU Private: 1 indicates the cache in L2 IU_Present is private per core
		 * - Bit 6: L2 IU Inclusive: 1 indicates L2 IU cache is inclusive of L1
		 * - Bit 7: L2 D Present: 1 indicates L2 data cache is present
		 * - Bit 8: L2 D Private: 1 indicates L2 data cache is private per core
		 * - Bit 9: L2 D Inclusive: 1 indicates L2 data cache inclusive of L1
		 * - Bit 10: L3 IU Present: 1 indicates L3 instruction cache or L3 system cache is present
		 * - Bit 11: L3 IU Unify: 1 indicates the cache in L3 IU_Present is unified
		 * - Bit 12: L3 IU Private: 1 indicates the cache in L3 IU_Present is private per core
		 * - Bit 13: L3 IU Inclusive: 1 indicates L3 IU cache inclusive of L1+L2
		 * - Bit 14: L3 D Present: 1 indicates L3 data cache is present
		 * - Bit 15: L3 D Private: 1 indicates L3 data cache is private per core
		 * - Bit 16: L3 D Inclusive: 1 indicates L3 data cache inclusive of lower levels
		 */
		Rocinante::Optional<uint32_t> m_word10;

		/**
		 * @brief Word number 0x11: Cache geometry for cache corresponding to L1 IU_Present (word 0x10 bit 0)
		 *
		 * - Bits 15:0: Way-1: number of ways minus 1
		 * - Bits 23:16: Index-log2: log2(number of cache rows per way)
		 * - Bits 30:24: Linesize-log2: log2(cache line size in bytes)
		 */
		Rocinante::Optional<uint32_t> m_word11;

		/**
		 * @brief Word number 0x12: Cache geometry for cache corresponding to L1 D Present (word 0x10 bit 2)
		 *
		 * - Bits 15:0: Way-1: number of ways minus 1
		 * - Bits 23:16: Index-log2: log2(number of cache rows per way)
		 * - Bits 30:24: Linesize-log2: log2(cache line size in bytes)
		 */
		Rocinante::Optional<uint32_t> m_word12;

		/**
		 * @brief Word number 0x13: Cache geometry for cache corresponding to L2 IU Present (word 0x10 bit 3)
		 *
		 * - Bits 15:0: Way-1: number of ways minus 1
		 * - Bits 23:16: Index-log2: log2(number of cache rows per way)
		 * - Bits 30:24: Linesize-log2: log2(cache line size in bytes)
		 */
		Rocinante::Optional<uint32_t> m_word13;

		/**
		 * @brief Word number 0x14: Cache geometry for cache corresponding to L3 IU Present (word 0x10 bit 10)
		 *
		 * - Bits 15:0: Way-1: number of ways minus 1
		 * - Bits 23:16: Index-log2: log2(number of cache rows per way)
		 * - Bits 30:24: Linesize-log2: log2(cache line size in bytes)
		 */
		Rocinante::Optional<uint32_t> m_word14;

	public:
		CPUCFG() = default;

		uint32_t Word(uint32_t word_number) { return _word(word_number); }

		uint32_t ProcessorID() {
			return _word(0x0);
		}

		Architecture Arch() {
			return static_cast<Architecture>(_bits(_word(0x1), 1, 0));
		}

		bool MMUSupportsPageMappingMode() {
			return _bit(_word(0x1), 2);
		}

		bool SupportsIOCSR() {
			return _bit(_word(0x1), 3);
		}

		uint32_t PALENMinus1() { return _bits(_word(0x1), 11, 4); }
		uint32_t VALENMinus1() { return _bits(_word(0x1), 19, 12); }
		uint32_t PhysicalAddressBits() { return PALENMinus1() + 1; }
		uint32_t VirtualAddressBits() { return VALENMinus1() + 1; }

		bool SupportsUnalignedAccess() { return _bit(_word(0x1), 20); }
		bool SupportsReadInhibit() { return _bit(_word(0x1), 21); }
		bool SupportsExecProtection() { return _bit(_word(0x1), 22); }
		bool SupportsRPLV() { return _bit(_word(0x1), 23); }
		bool SupportsHugePage() { return _bit(_word(0x1), 24); }
		bool SupportsCRC() { return _bit(_word(0x1), 25); }
		bool ExternalInterruptIsMessageInterruptMode() { return _bit(_word(0x1), 26); }

		bool SupportsFP() { return _bit(_word(0x2), 0); }
		bool SupportsSinglePrecisionFP() { return _bit(_word(0x2), 1); }
		bool SupportsDoublePrecisionFP() { return _bit(_word(0x2), 2); }
		uint32_t FPVersion() { return _bits(_word(0x2), 5, 3); }
		bool SupportsLSX() { return _bit(_word(0x2), 6); }
		bool SupportsLASX() { return _bit(_word(0x2), 7); }
		bool SupportsComplexVector() { return _bit(_word(0x2), 8); }
		bool SupportsCryptoVector() { return _bit(_word(0x2), 9); }
		bool SupportsVirtualizationExtension() { return _bit(_word(0x2), 10); }
		uint32_t VirtualizationVersion() { return _bits(_word(0x2), 13, 11); }
		bool SupportsConstantFrequencyCounterTimer() { return _bit(_word(0x2), 14); }
		uint32_t ConstantFrequencyCounterTimerVersion() { return _bits(_word(0x2), 17, 15); }
		bool SupportsX86BinaryTranslation() { return _bit(_word(0x2), 18); }
		bool SupportsARMBinaryTranslation() { return _bit(_word(0x2), 19); }
		bool SupportsMIPSBinaryTranslation() { return _bit(_word(0x2), 20); }
		bool SupportsSoftwarePageTableWalkInstruction() { return _bit(_word(0x2), 21); }
		bool SupportsAMAtomicMemoryAccess() { return _bit(_word(0x2), 22); }
		bool SupportsPageTableWalker() { return _bit(_word(0x2), 24); }
		bool SupportsFRECIPEFRSQRTE() { return _bit(_word(0x2), 25); }
		bool DIVWMODWComputeOnlyLow32OnLA64() { return _bit(_word(0x2), 26); }
		bool SupportsAMBH() { return _bit(_word(0x2), 27); }
		bool SupportsLAMCAS() { return _bit(_word(0x2), 28); }
		bool SupportsLLACQSCREL() { return _bit(_word(0x2), 29); }
		bool SupportsSCQ() { return _bit(_word(0x2), 30); }

		bool SupportsCacheCoherentDMA() { return _bit(_word(0x3), 0); }
		bool SupportsStoreFillBuffer() { return _bit(_word(0x3), 1); }
		bool SupportsUCACCWindow() { return _bit(_word(0x3), 2); }
		bool SupportsLLExclusiveBlockFunction() { return _bit(_word(0x3), 3); }
		bool SupportsRandomDelayAfterSC() { return _bit(_word(0x3), 4); }
		bool SupportsLLAutomaticWithDBAR() { return _bit(_word(0x3), 5); }
		bool HardwareMaintainsITLBAndTLBConsistency() { return _bit(_word(0x3), 6); }
		bool HardwareMaintainsICacheAndDCacheConsistencyInCore() { return _bit(_word(0x3), 7); }
		uint32_t PageWalkMaxDirectoryLevels() { return _bits(_word(0x3), 10, 8); }
		bool PageWalkFillsTLBHalfOnLargePage() { return _bit(_word(0x3), 11); }
		bool SupportsShorteningVirtualAddressRange() { return _bit(_word(0x3), 12); }
		uint32_t RVAMAXMinus1() { return _bits(_word(0x3), 16, 13); }
		bool DBARNonZeroHintsImplementedAsRecommended() { return _bit(_word(0x3), 17); }
		bool HardwareGuaranteesSequentialLoadsSameAddress() { return _bit(_word(0x3), 23); }

		uint32_t ConstantFrequencyCounterCrystalFrequency() { return _word(0x4); }
		uint32_t ConstantFrequencyCounterMul() { return _bits(_word(0x5), 15, 0); }
		uint32_t ConstantFrequencyCounterDiv() { return _bits(_word(0x5), 31, 16); }

		bool SupportsPerformanceMonitor() { return _bit(_word(0x6), 0); }
		uint32_t PerformanceMonitorEventVersion() { return _bits(_word(0x6), 3, 1); }
		uint32_t PerformanceMonitorCountMinus1() { return _bits(_word(0x6), 7, 4); }
		uint32_t PerformanceMonitorBitsMinus1() { return _bits(_word(0x6), 13, 8); }
		bool UserModePerformanceMonitorAccess() { return _bit(_word(0x6), 14); }

		bool L1IUPresent() { return _bit(_word(0x10), 0); }
		bool L1IUUnified() { return _bit(_word(0x10), 1); }
		bool L1DPresent() { return _bit(_word(0x10), 2); }
		bool L2IUPresent() { return _bit(_word(0x10), 3); }
		bool L2IUUnified() { return _bit(_word(0x10), 4); }
		bool L2IUPrivate() { return _bit(_word(0x10), 5); }
		bool L2IUInclusive() { return _bit(_word(0x10), 6); }
		bool L2DPresent() { return _bit(_word(0x10), 7); }
		bool L2DPrivate() { return _bit(_word(0x10), 8); }
		bool L2DInclusive() { return _bit(_word(0x10), 9); }
		bool L3IUPresent() { return _bit(_word(0x10), 10); }
		bool L3IUUnified() { return _bit(_word(0x10), 11); }
		bool L3IUPrivate() { return _bit(_word(0x10), 12); }
		bool L3IUInclusive() { return _bit(_word(0x10), 13); }
		bool L3DPresent() { return _bit(_word(0x10), 14); }
		bool L3DPrivate() { return _bit(_word(0x10), 15); }
		bool L3DInclusive() { return _bit(_word(0x10), 16); }

		Rocinante::Optional<CacheGeometry> L1IUGeometry() {
			if (!L1IUPresent()) return Rocinante::nullopt;
			const uint32_t w = _word(0x11);
			return CacheGeometry{
				.WaysMinus1 = static_cast<uint16_t>(_bits(w, 15, 0)),
				.IndexLog2 = static_cast<uint8_t>(_bits(w, 23, 16)),
				.LineSizeLog2 = static_cast<uint8_t>(_bits(w, 30, 24)),
			};
		}

		Rocinante::Optional<CacheGeometry> L1DGeometry() {
			if (!L1DPresent()) return Rocinante::nullopt;
			const uint32_t w = _word(0x12);
			return CacheGeometry{
				.WaysMinus1 = static_cast<uint16_t>(_bits(w, 15, 0)),
				.IndexLog2 = static_cast<uint8_t>(_bits(w, 23, 16)),
				.LineSizeLog2 = static_cast<uint8_t>(_bits(w, 30, 24)),
			};
		}

		Rocinante::Optional<CacheGeometry> L2IUGeometry() {
			if (!L2IUPresent()) return Rocinante::nullopt;
			const uint32_t w = _word(0x13);
			return CacheGeometry{
				.WaysMinus1 = static_cast<uint16_t>(_bits(w, 15, 0)),
				.IndexLog2 = static_cast<uint8_t>(_bits(w, 23, 16)),
				.LineSizeLog2 = static_cast<uint8_t>(_bits(w, 30, 24)),
			};
		}

		Rocinante::Optional<CacheGeometry> L3IUGeometry() {
			if (!L3IUPresent()) return Rocinante::nullopt;
			const uint32_t w = _word(0x14);
			return CacheGeometry{
				.WaysMinus1 = static_cast<uint16_t>(_bits(w, 15, 0)),
				.IndexLog2 = static_cast<uint8_t>(_bits(w, 23, 16)),
				.LineSizeLog2 = static_cast<uint8_t>(_bits(w, 30, 24)),
			};
		}
};

/**
 * @brief Single, canonical instance of CPUCFG that can be used throughout the kernel
 * to query CPU configuration.
 * 
 * @return CPUCFG& Reference to the single instance of CPUCFG.
 */
static inline CPUCFG& GetCPUCFG() {
	static CPUCFG instance;
	return instance;
}

} // namespace Rocinante

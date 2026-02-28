/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "kernel.h"

#include <src/memory/memory.h>
#include <src/memory/boot_memory_map.h>
#include <src/memory/pmm.h>
#include <src/memory/paging.h>
#include <src/memory/paging_hw.h>
#include <src/memory/virtual_layout.h>
#include <src/sp/cpucfg.h>
#include <src/sp/uart16550.h>
#include <src/sp/mmio.h>
#include <src/helpers/optional.h>
#include <src/trap.h>

#include <src/testing/test.h>

namespace {

constexpr std::uintptr_t UART_BASE = 0x1fe001e0UL; // QEMU LoongArch virt: VIRT_UART_BASE address
constexpr std::uintptr_t kSysconBase = 0x100e001cUL; // QEMU LoongArch virt: syscon-poweroff MMIO base

static constinit Rocinante::Uart16550 uart(UART_BASE);

namespace Csr {
	// LoongArch privileged architecture CSR numbering.
	constexpr std::uint32_t kTlbIndex = 0x10;   // CSR.TLBIDX
	constexpr std::uint32_t kTlbEntryHigh = 0x11; // CSR.TLBEHI
	constexpr std::uint32_t kAddressSpaceId = 0x18; // CSR.ASID
	constexpr std::uint32_t kPgdLow = 0x19;     // CSR.PGDL
	constexpr std::uint32_t kPgdHigh = 0x1A;    // CSR.PGDH
	constexpr std::uint32_t kPgd = 0x1B;        // CSR.PGD (read-only)
	constexpr std::uint32_t kPageWalkControlLow = 0x1C;  // CSR.PWCL
	constexpr std::uint32_t kPageWalkControlHigh = 0x1D; // CSR.PWCH
	constexpr std::uint32_t kReducedVirtualAddressConfiguration = 0x1F; // CSR.RVACFG

	constexpr std::uint32_t kTlbRefillEntryAddress = 0x88; // CSR.TLBRENTRY
	constexpr std::uint32_t kTlbRefillBadVirtualAddress = 0x89; // CSR.TLBRBADV
	constexpr std::uint32_t kTlbRefillExceptionReturnAddress = 0x8A; // CSR.TLBRERA
	constexpr std::uint32_t kTlbRefillEntryHigh = 0x8E; // CSR.TLBREHI
} // namespace Csr

template<std::uint32_t CsrNumber>
static inline std::uint64_t ReadCsr() {
	std::uint64_t value;
	asm volatile("csrrd %0, %1" : "=r"(value) : "i"(CsrNumber));
	return value;
}

static const char* ExceptionCodeToString(std::uint64_t exception_code, std::uint64_t exception_subcode) {
	// Table 21 "Table of exception encoding" (LoongArch Privileged Architecture).
	switch (exception_code) {
		case 0x0: return "INT";
		case 0x1: return "PIL";
		case 0x2: return "PIS";
		case 0x3: return "PIF";
		case 0x4: return "PME";
		case 0x5: return "PNR";
		case 0x6: return "PNX";
		case 0x7: return "PPI";
		case 0x8:
			if (exception_subcode == 0) return "ADEF";
			if (exception_subcode == 1) return "ADEM";
			return "AD";
		case 0x9: return "ALE";
		case 0xA: return "BCE";
		case 0xB: return "SYS";
		case 0x0c: return "BRK";
		case 0xD: return "INE";
		case 0xE: return "IPE";
		case 0xF: return "FPD";
		case 0x10: return "SXD";
		case 0x11: return "ASXD";
		case 0x12:
			if (exception_subcode == 0) return "FPE";
			if (exception_subcode == 1) return "VFPE";
			return "FPE";
		default: return "UNKNOWN";
	}
}

static void DumpMappedTranslationCsrs(const Rocinante::Uart16550& uart) {
	const std::uint64_t pgdl = ReadCsr<Csr::kPgdLow>();
	const std::uint64_t pgdh = ReadCsr<Csr::kPgdHigh>();
	const std::uint64_t pgd = ReadCsr<Csr::kPgd>();
	const std::uint64_t pwcl = ReadCsr<Csr::kPageWalkControlLow>();
	const std::uint64_t pwch = ReadCsr<Csr::kPageWalkControlHigh>();
	const std::uint64_t rvacfg = ReadCsr<Csr::kReducedVirtualAddressConfiguration>();

	uart.puts("PGDL:  "); uart.write_hex_u64(pgdl); uart.putc('\n');
	uart.puts("PGDH:  "); uart.write_hex_u64(pgdh); uart.putc('\n');
	uart.puts("PGD:   "); uart.write_hex_u64(pgd); uart.putc('\n');
	uart.puts("PWCL:  "); uart.write_hex_u64(pwcl); uart.putc('\n');
	uart.puts("PWCH:  "); uart.write_hex_u64(pwch); uart.putc('\n');
	uart.puts("RVACFG:"); uart.write_hex_u64(rvacfg); uart.putc('\n');
}

static void DumpTlbRefillCsrsIfActive(const Rocinante::Uart16550& uart) {
	const std::uint64_t tlbrera = ReadCsr<Csr::kTlbRefillExceptionReturnAddress>();
	const bool is_tlbr = (tlbrera & 1ull) != 0;
	if (!is_tlbr) return;

	const std::uint64_t tlbrbadv = ReadCsr<Csr::kTlbRefillBadVirtualAddress>();
	const std::uint64_t tlbrehi = ReadCsr<Csr::kTlbRefillEntryHigh>();
	const std::uint64_t tlbrentry = ReadCsr<Csr::kTlbRefillEntryAddress>();

	uart.puts("TLBR:  IsTLBR=1\n");
	uart.puts("TLBRENTRY:"); uart.write_hex_u64(tlbrentry); uart.putc('\n');
	uart.puts("TLBRERA:  "); uart.write_hex_u64(tlbrera); uart.putc('\n');
	uart.puts("TLBRBADV: "); uart.write_hex_u64(tlbrbadv); uart.putc('\n');
	uart.puts("TLBREHI:  "); uart.write_hex_u64(tlbrehi); uart.putc('\n');
}

static const char* BootMemoryRegionTypeToString(Rocinante::Memory::BootMemoryRegion::Type type) {
	switch (type) {
		case Rocinante::Memory::BootMemoryRegion::Type::UsableRAM:
			return "UsableRAM";
		case Rocinante::Memory::BootMemoryRegion::Type::Reserved:
			return "Reserved";
	}
	return "Unknown";
}

static void PrintBootMemoryMap(const Rocinante::Uart16550& uart, const Rocinante::Memory::BootMemoryMap& map) {
	uart.puts("Boot memory map (DTB):\n");
	uart.puts("  Region count: ");
	uart.write(Rocinante::to_string(map.region_count));
	uart.putc('\n');

	for (std::size_t i = 0; i < map.region_count; i++) {
		const auto& r = map.regions[i];
		uart.puts("  - ");
		uart.puts(BootMemoryRegionTypeToString(r.type));
		uart.puts(" base=");
		uart.write(Rocinante::to_string(r.physical_base));
		uart.puts(" size_bytes=");
		uart.write(Rocinante::to_string(r.size_bytes));
		uart.putc('\n');
	}
}

static void PrintPhysicalMemoryManagerSummary(const Rocinante::Uart16550& uart, const Rocinante::Memory::PhysicalMemoryManager& pmm) {
	uart.puts("PMM summary:\n");
	uart.puts("  Tracked physical base:  ");
	uart.write(Rocinante::to_string(pmm.TrackedPhysicalBase()));
	uart.putc('\n');
	uart.puts("  Tracked physical limit: ");
	uart.write(Rocinante::to_string(pmm.TrackedPhysicalLimit()));
	uart.putc('\n');
	uart.puts("  Total pages: ");
	uart.write(Rocinante::to_string(pmm.TotalPages()));
	uart.putc('\n');
	uart.puts("  Free pages:  ");
	uart.write(Rocinante::to_string(pmm.FreePages()));
	uart.putc('\n');
}

static const void* TryLocateDeviceTreeBlobPointerFromBootInfoRegion() {
	// QEMU's direct-kernel boot commonly places the DTB in low physical memory.
	// Our linker script intentionally keeps the kernel image clear of this area.
	//
	// We do not yet parse the EFI system table to locate FDT/ACPI tables.
	// For current bring-up (especially QEMU direct-kernel boot), we use this
	// scan as a heuristic to locate a valid FDT header in the conventional
	// low-memory "boot info" area.
	// Search range policy:
	// - Start at 0x4 instead of 0x0 so we never pass a null pointer.
	// - Search the first 16 MiB, which is a common area for firmware/boot blobs.
	static constexpr std::uintptr_t kSearchBeginPhysical = 0x00000004UL;
	static constexpr std::uintptr_t kSearchEndPhysical = 0x01000000UL;
	static constexpr std::size_t kSearchStepBytes = 4;

	for (std::uintptr_t candidate = kSearchBeginPhysical; (candidate + 4) < kSearchEndPhysical; candidate += kSearchStepBytes) {
		const void* p = reinterpret_cast<const void*>(candidate);
		if (!Rocinante::Memory::BootMemoryMap::LooksLikeDeviceTreeBlob(p)) continue;

		const std::size_t total_size_bytes = Rocinante::Memory::BootMemoryMap::DeviceTreeTotalSizeBytesOrZero(p);
		if (total_size_bytes == 0) continue;
		if ((candidate + total_size_bytes) > kSearchEndPhysical) continue;

		return p;
	}

	return nullptr;
}

[[noreturn]] static inline void halt() {
	for (;;) {
		asm volatile("idle 0" ::: "memory");
	}
}

[[noreturn]] [[maybe_unused]] static inline void shutdown() {
	// QEMU LoongArch64 virt poweroff is wired up as a "syscon-poweroff" device.
	// The virt machine advertises this via its DTB:
	// - /poweroff compatible = "syscon-poweroff"
	// - regmap -> syscon at 0x100e001c (reg-io-width = 1)
	// - offset = 0, value = 0x34
	// Writing that byte triggers a QEMU shutdown event, and QEMU exits by default
	// (-action shutdown=poweroff).
	constexpr std::uintptr_t kPoweroffOffset = 0;
	constexpr std::uint8_t kPoweroffValue = 0x34;

	Rocinante::MMIO<8>::write(kSysconBase + kPoweroffOffset, kPoweroffValue);
	asm volatile("dbar 0" ::: "memory");

	// If QEMU ignores the poweroff request, just stop.
	halt();
}


} // namespace

extern "C" char _start;
extern "C" char _end;

extern "C" void RocinanteTrapHandler(Rocinante::TrapFrame* tf) {
	const std::uint64_t exception_code =
		Rocinante::Trap::ExceptionCodeFromExceptionStatus(tf->exception_status);
	const std::uint64_t exception_subcode =
		Rocinante::Trap::ExceptionSubCodeFromExceptionStatus(tf->exception_status);
	const std::uint64_t interrupt_status =
		Rocinante::Trap::InterruptStatusFromExceptionStatus(tf->exception_status);

	#if defined(ROCINANTE_TESTS)
	if (Rocinante::Testing::HandleTrap(tf, exception_code, exception_subcode, interrupt_status)) {
		return;
	}
	#else

	// LoongArch EXCCODE values (subset used for early bring-up).
	constexpr std::uint64_t kExceptionCodeBreak = 0x0c;
	constexpr std::uint64_t kTimerInterruptLineBit = (1ull << 11);

	// Interrupts arrive with EXCCODE=0 and the pending lines in ESTAT.IS.
	if (exception_code == 0 && (interrupt_status & kTimerInterruptLineBit) != 0) {
		Rocinante::Trap::ClearTimerInterrupt();
		Rocinante::Trap::StopTimer();
		return;
	}

	if (exception_code == kExceptionCodeBreak) {
		uart.puts("\n*** TRAP: BRK ***\n");
		uart.puts("ERA:   "); uart.write_hex_u64(tf->exception_return_address); uart.putc('\n');
		uart.puts("ESTAT: "); uart.write_hex_u64(tf->exception_status); uart.putc('\n');
		uart.puts("SUB:   "); uart.write_dec_u64(exception_subcode); uart.putc('\n');

		// Skip the BREAK instruction so we can prove ERTN return works.
		// LoongArch instructions are 32-bit.
		tf->exception_return_address += 4;
		return;
	}
	#endif

	uart.puts("\n*** TRAP ***\n");
	uart.puts("TYPE:  ");
	uart.puts(ExceptionCodeToString(exception_code, exception_subcode));
	uart.puts(" (EXC="); uart.write_hex_u64(exception_code);
	uart.puts(" SUB="); uart.write_hex_u64(exception_subcode);
	uart.puts(")\n");

	uart.puts("ERA:   "); uart.write_hex_u64(tf->exception_return_address); uart.putc('\n');
	uart.puts("ESTAT: "); uart.write_hex_u64(tf->exception_status); uart.putc('\n');
	uart.puts("BADV:  "); uart.write_hex_u64(tf->bad_virtual_address); uart.putc('\n');
	uart.puts("CRMD:  "); uart.write_hex_u64(tf->current_mode_information); uart.putc('\n');
	uart.puts("PRMD:  "); uart.write_hex_u64(tf->previous_mode_information); uart.putc('\n');
	uart.puts("ECFG:  "); uart.write_hex_u64(tf->exception_configuration); uart.putc('\n');

	DumpTlbRefillCsrsIfActive(uart);
	DumpMappedTranslationCsrs(uart);

	// A few extra CSRs that are useful when debugging translation state.
	uart.puts("ASID:  "); uart.write_hex_u64(ReadCsr<Csr::kAddressSpaceId>()); uart.putc('\n');
	uart.puts("TLBIDX:"); uart.write_hex_u64(ReadCsr<Csr::kTlbIndex>()); uart.putc('\n');
	uart.puts("TLBEHI:"); uart.write_hex_u64(ReadCsr<Csr::kTlbEntryHigh>()); uart.putc('\n');

	halt();
}

extern "C" [[noreturn]] void kernel_main(std::uint64_t is_uefi_compliant_bootenv, std::uint64_t kernel_cmdline_ptr, std::uint64_t boot_info_ptr_a2) {
	Rocinante::Memory::InitEarly();
	Rocinante::Trap::Initialize();

	uart.puts("Boot args (raw): a0=");
	uart.write(Rocinante::to_string(is_uefi_compliant_bootenv));
	uart.puts(" a1=");
	uart.write(Rocinante::to_string(kernel_cmdline_ptr));
	uart.puts(" a2=");
	uart.write(Rocinante::to_string(boot_info_ptr_a2));
	uart.putc('\n');

	// Read the kernel command line from the pointer passed in a1 by the boot environment, if present.
	if (kernel_cmdline_ptr) {
		const char* cmdline = reinterpret_cast<const char*>(kernel_cmdline_ptr);
		uart.puts("Kernel command line: ");
		uart.puts(cmdline);
		uart.putc('\n');
	}

	#if defined(ROCINANTE_TESTS)
	const int failed = Rocinante::Testing::RunAll(&uart);
	if (failed == 0) {
		uart.puts("\nALL TESTS PASSED\n");
	} else {
		uart.puts("\nTESTS FAILED\n");
	}
	shutdown();
	#endif

	// Flaw / bring-up gap:
	// We do not yet implement EFI system table parsing to locate ACPI/FDT tables.
	// For QEMU direct-kernel bring-up, we therefore rely on a heuristic: scan low
	// physical memory for a structurally-valid DTB.
	const void* maybe_device_tree_blob = TryLocateDeviceTreeBlobPointerFromBootInfoRegion();

	if (maybe_device_tree_blob) {
		const std::uintptr_t device_tree_physical_base = reinterpret_cast<std::uintptr_t>(maybe_device_tree_blob);
		const std::size_t device_tree_size_bytes = Rocinante::Memory::BootMemoryMap::DeviceTreeTotalSizeBytesOrZero(maybe_device_tree_blob);
		uart.puts("DTB detected: base=");
		uart.write(Rocinante::to_string(device_tree_physical_base));
		uart.puts(" size_bytes=");
		uart.write(Rocinante::to_string(device_tree_size_bytes));
		uart.puts(" source=scan(low-mem)");
		uart.putc('\n');

		Rocinante::Memory::BootMemoryMap boot_map;
		if (boot_map.TryParseFromDeviceTree(maybe_device_tree_blob)) {
			PrintBootMemoryMap(uart, boot_map);

			const std::uintptr_t kernel_physical_base = reinterpret_cast<std::uintptr_t>(&_start);
			const std::uintptr_t kernel_physical_end = reinterpret_cast<std::uintptr_t>(&_end);

			auto& pmm = Rocinante::Memory::GetPhysicalMemoryManager();
			if (pmm.InitializeFromBootMemoryMap(
				boot_map,
				kernel_physical_base,
				kernel_physical_end,
				device_tree_physical_base,
				device_tree_size_bytes
			)) {
				PrintPhysicalMemoryManagerSummary(uart, pmm);

				// Paging bring-up is intentionally compile-time gated.
				//
				// Flaw / bring-up gap:
				// - We have not yet validated our page table entry encoding against the
				//   LoongArch privileged spec end-to-end.
					// - We do not yet establish a higher-half kernel. We do, however, build a
					//   minimal higher-half physmap so future paging code can access physical
					//   frames via a stable VA once paging is enabled.
				// - Therefore, default behavior is to build page tables and print diagnostics
				//   only; enabling paging requires opting in.
				#if defined(ROCINANTE_PAGING_BRINGUP)
				uart.puts("\nPaging bring-up: building bootstrap page tables\n");

				const std::uint32_t virtual_address_bits = Rocinante::GetCPUCFG().VirtualAddressBits();
				const std::uint32_t physical_address_bits = Rocinante::GetCPUCFG().PhysicalAddressBits();
				uart.puts("Paging bring-up: CPUCFG VALEN=");
				uart.write(Rocinante::to_string(virtual_address_bits));
				uart.puts(" PALEN=");
				uart.write(Rocinante::to_string(physical_address_bits));
				uart.putc('\n');

				constexpr auto BitIndexFromSingleBitMask = [](std::uint64_t mask) constexpr -> std::uint8_t {
					std::uint8_t index = 0;
					while (((mask >> index) & 0x1ull) == 0) {
						index++;
					}
					return index;
				};
				constexpr std::uint8_t kLowestHighFlagBit =
					(BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoRead) <
						BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoExecute))
						? BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoRead)
						: BitIndexFromSingleBitMask(Rocinante::Memory::Paging::PteBits::kNoExecute);
				constexpr std::uint32_t kMaxEncodablePALEN = kLowestHighFlagBit;
				if (physical_address_bits < Rocinante::Memory::Paging::kPageShiftBits || physical_address_bits > kMaxEncodablePALEN) {
					uart.puts("Paging bring-up: unsupported PALEN for current PTE encoding; skipping.\n");
				} else {
					const Rocinante::Memory::Paging::AddressSpaceBits address_bits{
						.virtual_address_bits = static_cast<std::uint8_t>(virtual_address_bits),
						.physical_address_bits = static_cast<std::uint8_t>(physical_address_bits),
					};

					const auto root_or = Rocinante::Memory::Paging::AllocateRootPageTable(&pmm);
					if (!root_or.has_value()) {
						uart.puts("Paging bring-up: failed to allocate root page table\n");
					} else {
						const auto root = root_or.value();

						// Identity-map the kernel image so enabling paging does not immediately
						// fault while executing in the low physical mapping.
						const std::uintptr_t kernel_size_bytes = kernel_physical_end - kernel_physical_base;
						const std::size_t map_size_rounded =
							static_cast<std::size_t>((kernel_size_bytes + Rocinante::Memory::Paging::kPageSizeBytes - 1) &
									~(Rocinante::Memory::Paging::kPageSizeBytes - 1));

						Rocinante::Memory::Paging::PagePermissions kernel_permissions{
							.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
							.execute = Rocinante::Memory::Paging::ExecutePermissions::Executable,
							.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
							.global = true,
						};

						if (!Rocinante::Memory::Paging::MapRange4KiB(
							&pmm,
							root,
							kernel_physical_base,
							kernel_physical_base,
							map_size_rounded,
							kernel_permissions,
							address_bits
						)) {
							uart.puts("Paging bring-up: failed to map kernel identity range\n");
						} else {
							// Identity-map the UART and syscon MMIO pages so existing raw MMIO pointers
							// remain usable immediately after paging is enabled.
							//
							// Correctness pitfall (per plan / LoongArch spec): caching attributes.
							// These must be mapped as an uncached/strongly-ordered memory type, or
							// early UART/debug output can become flaky.
							Rocinante::Memory::Paging::PagePermissions mmio_permissions{
								.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
								.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
								.cache = Rocinante::Memory::Paging::CacheMode::StrongUncached,
								.global = true,
							};

							const std::uintptr_t uart_page_base =
								UART_BASE & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
							if (!Rocinante::Memory::Paging::MapRange4KiB(
								&pmm,
								root,
								uart_page_base,
								uart_page_base,
								Rocinante::Memory::Paging::kPageSizeBytes,
								mmio_permissions,
								address_bits
							)) {
								uart.puts("Paging bring-up: failed to map UART MMIO page\n");
							}

							const std::uintptr_t syscon_page_base =
								kSysconBase & ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
							if (!Rocinante::Memory::Paging::MapRange4KiB(
								&pmm,
								root,
								syscon_page_base,
								syscon_page_base,
								Rocinante::Memory::Paging::kPageSizeBytes,
								mmio_permissions,
								address_bits
							)) {
								uart.puts("Paging bring-up: failed to map syscon-poweroff MMIO page\n");
							}

							// Minimal physmap: map a small linear window of physical RAM into the
							// higher half so page-table pages and PMM frames can be accessed by VA
							// once paging is enabled.
							//
							// Bring-up policy:
							// - Keep this deliberately small at first.
							// - Start from the PMM tracked base (not necessarily 0).
							static constexpr std::size_t kBootstrapPhysMapSizeBytes = 16u * 1024u * 1024u; // 16 MiB

							const std::uintptr_t physmap_physical_base = pmm.TrackedPhysicalBase();
							const std::uintptr_t physmap_physical_limit = pmm.TrackedPhysicalLimit();
							std::size_t physmap_size_bytes = 0;
							if (physmap_physical_limit > physmap_physical_base) {
								const std::size_t tracked_size_bytes =
									static_cast<std::size_t>(physmap_physical_limit - physmap_physical_base);
								physmap_size_bytes = (tracked_size_bytes < kBootstrapPhysMapSizeBytes)
									? tracked_size_bytes
									: kBootstrapPhysMapSizeBytes;
								physmap_size_bytes &= ~(Rocinante::Memory::Paging::kPageSizeBytes - 1);
							}

							if (physmap_size_bytes == 0) {
								uart.puts("Paging bring-up: no tracked RAM for physmap; skipping physmap build\n");
							} else {
								const std::uintptr_t physmap_virtual_base =
									Rocinante::Memory::VirtualLayout::ToPhysMapVirtual(
										physmap_physical_base,
										address_bits.virtual_address_bits
									);

								Rocinante::Memory::Paging::PagePermissions physmap_permissions{
									.access = Rocinante::Memory::Paging::AccessPermissions::ReadWrite,
									.execute = Rocinante::Memory::Paging::ExecutePermissions::NoExecute,
									.cache = Rocinante::Memory::Paging::CacheMode::CoherentCached,
									.global = true,
								};

								if (!Rocinante::Memory::Paging::MapRange4KiB(
									&pmm,
									root,
									physmap_virtual_base,
									physmap_physical_base,
									physmap_size_bytes,
									physmap_permissions,
									address_bits
								)) {
									uart.puts("Paging bring-up: failed to map bootstrap physmap window\n");
								} else {
									uart.puts("Paging bring-up: physmap virt_base=");
									uart.write(Rocinante::to_string(physmap_virtual_base));
									uart.puts(" phys=[");
									uart.write(Rocinante::to_string(physmap_physical_base));
									uart.puts(", ");
									uart.write(Rocinante::to_string(physmap_physical_base + physmap_size_bytes));
									uart.puts(")\n");
								}
							}

							uart.puts("Paging bring-up: root_pt_phys=");
							uart.write(Rocinante::to_string(root.root_physical_address));
							uart.puts(" kernel_phys=[");
							uart.write(Rocinante::to_string(kernel_physical_base));
							uart.puts(", ");
							uart.write(Rocinante::to_string(kernel_physical_end));
							uart.puts(")\n");

							// Configure the hardware page walker but do not enable paging unless
							// explicitly requested.
							const bool enable_ptw = Rocinante::GetCPUCFG().SupportsPageTableWalker();
							const auto config_or = Rocinante::Memory::PagingHw::Make4KiBPageWalkerConfig(address_bits);
							if (!config_or.has_value()) {
								uart.puts("Paging bring-up: VALEN cannot be encoded in PWCL/PWCH for 4 KiB paging; skipping HW config.\n");
							} else {
								Rocinante::Memory::PagingHw::ConfigurePageTableWalker(root, config_or.value());
								uart.puts("Paging bring-up: configured PWCL/PWCH/PGD CSRs (CPUCFG.HPTW=");
								uart.puts(enable_ptw ? "on" : "off");
								uart.puts(")\n");

								#if defined(ROCINANTE_ENABLE_PAGING_NOW)
								uart.puts("Paging bring-up: enabling paging (CRMD.PG=1, CRMD.DA=0)\n");
								Rocinante::Memory::PagingHw::EnablePaging();
								uart.puts("Paging bring-up: paging enabled\n");
								#endif
							}
						}
					}
				}
				}
				#endif
			} else {
				uart.puts("Failed to initialize PMM from boot memory map\n");
			}
		} else {
			uart.puts("DTB detected but failed to parse boot memory map\n");
		}
	} else {
		uart.puts("No DTB detected; skipping boot memory map parse\n");
	}

	auto& cpucfg = Rocinante::GetCPUCFG();

	Rocinante::String info;
	info += "Hello, Rocinante!\n";
	info += "Don the LoongArch64 armor and prepare to ride!\n\n";

	uart.write(info);

	// Print some information about the CPU configuration using the CPUCFG class
	uart.puts("CPU Architecture: ");
	switch (cpucfg.Arch()) {
		case Rocinante::CPUCFG::Architecture::SimplifiedLA32:
			uart.puts("Simplified LA32\n");
			break;
		case Rocinante::CPUCFG::Architecture::LA32:
			uart.puts("LA32\n");
			break;
		case Rocinante::CPUCFG::Architecture::LA64:
			uart.puts("LA64\n");
			break;
		case Rocinante::CPUCFG::Architecture::Reserved:
			uart.puts("Reserved/Unknown\n");
			break;
	}

	uart.putc('\n');

	if (cpucfg.MMUSupportsPageMappingMode()) {
		uart.puts("MMU supports page mapping mode\n");
	} else {
		uart.puts("MMU does not support page mapping mode\n");
	}

	uart.putc('\n');

	// Let's read and print VALEN/PALEN as a sanity check that our CPUCFG class is working and we can read CPU-reported information correctly.
	uart.puts("Supported virtual address bits (VALEN): ");
	uart.write(Rocinante::to_string(cpucfg.VirtualAddressBits()));
	uart.putc('\n');
	uart.puts("Supported physical address bits (PALEN): ");
	uart.write(Rocinante::to_string(cpucfg.PhysicalAddressBits()));
	uart.putc('\n');

	uart.putc('\n');

	halt();
}

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

static constinit Rocinante::Uart16550 uart(UART_BASE);

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
	constexpr std::uintptr_t kSysconBase = 0x100e001cUL;
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
		uart.puts("ERA:   ");
		uart.write(Rocinante::to_string(tf->exception_return_address));
		uart.putc('\n');
		uart.puts("ESTAT: ");
		uart.write(Rocinante::to_string(tf->exception_status));
		uart.putc('\n');
		uart.puts("SUB:   ");
		uart.write(Rocinante::to_string(exception_subcode));
		uart.putc('\n');

		// Skip the BREAK instruction so we can prove ERTN return works.
		// LoongArch instructions are 32-bit.
		tf->exception_return_address += 4;
		return;
	}
	#endif

	uart.puts("\n*** TRAP ***\n");
	uart.puts("ERA:   ");
	uart.write(Rocinante::to_string(tf->exception_return_address));
	uart.putc('\n');
	uart.puts("ESTAT: ");
	uart.write(Rocinante::to_string(tf->exception_status));
	uart.putc('\n');
	uart.puts("BADV:  ");
	uart.write(Rocinante::to_string(tf->bad_virtual_address));
	uart.putc('\n');
	uart.puts("EXC:   ");
	uart.write(Rocinante::to_string(exception_code));
	uart.puts(" SUB: ");
	uart.write(Rocinante::to_string(exception_subcode));
	uart.putc('\n');

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
				// - We also do not yet establish a full physmap or a higher-half kernel.
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

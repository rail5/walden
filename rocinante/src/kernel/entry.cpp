/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/kernel.h>

#include <src/boot/boot_print.h>
#include <src/boot/dtb_scan.h>
#include <src/kernel/paging_bringup.h>
#include <src/memory/memory.h>
#include <src/memory/pmm.h>
#include <src/platform/console.h>
#include <src/platform/power.h>
#include <src/sp/cpucfg.h>
#include <src/sp/uart16550.h>
#include <src/testing/test.h>
#include <src/trap/trap.h>

#include <cstdint>

namespace {

extern "C" char _start;
extern "C" char _end;

static void PrintCpuArchitecture(const Rocinante::Uart16550& uart, Rocinante::CPUCFG::Architecture arch) {
	uart.puts("CPU Architecture: ");
	if (arch == Rocinante::CPUCFG::Architecture::SimplifiedLA32) {
		uart.putc('S');
		uart.putc('i');
		uart.putc('m');
		uart.putc('p');
		uart.putc('l');
		uart.putc('i');
		uart.putc('f');
		uart.putc('i');
		uart.putc('e');
		uart.putc('d');
		uart.putc(' ');
		uart.putc('L');
		uart.putc('A');
		uart.putc('3');
		uart.putc('2');
		uart.putc('\n');
		return;
	}
	if (arch == Rocinante::CPUCFG::Architecture::LA32) {
		uart.putc('L');
		uart.putc('A');
		uart.putc('3');
		uart.putc('2');
		uart.putc('\n');
		return;
	}
	if (arch == Rocinante::CPUCFG::Architecture::LA64) {
		uart.putc('L');
		uart.putc('A');
		uart.putc('6');
		uart.putc('4');
		uart.putc('\n');
		return;
	}

	uart.putc('R');
	uart.putc('e');
	uart.putc('s');
	uart.putc('e');
	uart.putc('r');
	uart.putc('v');
	uart.putc('e');
	uart.putc('d');
	uart.putc('/');
	uart.putc('U');
	uart.putc('n');
	uart.putc('k');
	uart.putc('n');
	uart.putc('o');
	uart.putc('w');
	uart.putc('n');
	uart.putc('\n');
}

[[noreturn]] static void KernelMain_PostMemoryInitialization() {
	auto& uart = Rocinante::Platform::GetEarlyUart();
	auto& cpucfg = Rocinante::GetCPUCFG();

	uart.puts("Hello, Rocinante!\n");
	uart.puts("Don the LoongArch64 armor and prepare to ride!\n\n");

	PrintCpuArchitecture(uart, cpucfg.Arch());

	uart.putc('\n');

	if (cpucfg.MMUSupportsPageMappingMode()) {
		uart.puts("MMU supports page mapping mode\n");
	} else {
		uart.puts("MMU does not support page mapping mode\n");
	}

	uart.putc('\n');

	// Let's read and print VALEN/PALEN as a sanity check that our CPUCFG class is
	// working and we can read CPU-reported information correctly.
	uart.puts("Supported virtual address bits (VALEN): ");
	uart.write_dec_u64(cpucfg.VirtualAddressBits());
	uart.putc('\n');
	uart.puts("Supported physical address bits (PALEN): ");
	uart.write_dec_u64(cpucfg.PhysicalAddressBits());
	uart.putc('\n');

	uart.putc('\n');

	Rocinante::Platform::Halt();
}

} // namespace

extern "C" [[noreturn]] void kernel_main(std::uint64_t is_uefi_compliant_bootenv, std::uint64_t kernel_cmdline_ptr, std::uint64_t boot_info_ptr_a2) {
	Rocinante::Memory::InitEarly();
	Rocinante::Trap::Initialize();

	auto& uart = Rocinante::Platform::GetEarlyUart();

	uart.puts("Boot args (raw): a0=");
	uart.write_dec_u64(is_uefi_compliant_bootenv);
	uart.puts(" a1=");
	uart.write_dec_u64(kernel_cmdline_ptr);
	uart.puts(" a2=");
	uart.write_dec_u64(boot_info_ptr_a2);
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
	Rocinante::Platform::Shutdown();
	#endif

	// Flaw / bring-up gap:
	// We do not yet implement EFI system table parsing to locate ACPI/FDT tables.
	// For QEMU direct-kernel bring-up, we therefore rely on a heuristic: scan low
	// physical memory for a structurally-valid DTB.
	const void* maybe_device_tree_blob = Rocinante::Boot::TryLocateDeviceTreeBlobPointerFromBootInfoRegion();

	if (maybe_device_tree_blob) {
		const std::uintptr_t device_tree_physical_base = reinterpret_cast<std::uintptr_t>(maybe_device_tree_blob);
		const std::size_t device_tree_size_bytes = Rocinante::Memory::BootMemoryMap::DeviceTreeTotalSizeBytesOrZero(maybe_device_tree_blob);
		uart.puts("DTB detected: base=");
		uart.write_dec_u64(device_tree_physical_base);
		uart.puts(" size_bytes=");
		uart.write_dec_u64(device_tree_size_bytes);
		uart.puts(" source=scan(low-mem)");
		uart.putc('\n');

		Rocinante::Memory::BootMemoryMap boot_map;
		if (boot_map.TryParseFromDeviceTree(maybe_device_tree_blob)) {
			Rocinante::Boot::PrintBootMemoryMap(uart, boot_map);

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
				Rocinante::Boot::PrintPhysicalMemoryManagerSummary(uart, pmm);

				// Paging is now the default boot path.
				//
				// Policy:
				// - Build bootstrap page tables.
				// - Switch into mapped address translation mode.
				// - Jump to the higher-half alias and continue normal kernel execution.
				Rocinante::Kernel::RunPagingBringup(
					uart,
					pmm,
					kernel_physical_base,
					kernel_physical_end,
					&KernelMain_PostMemoryInitialization
				);
			} else {
				uart.puts("Failed to initialize PMM from boot memory map\n");
			}
		} else {
			uart.puts("DTB detected but failed to parse boot memory map\n");
		}
	} else {
		uart.puts("No DTB detected; skipping boot memory map parse\n");
	}

	KernelMain_PostMemoryInitialization();
}

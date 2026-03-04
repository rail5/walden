# Makefile for building Rocinante kernel
PROJECT_ROOT_DIRECTORY = $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

## Directories and files
## Where to find sources, where to put objects, and what the final target is
SRCDIR     = src
BINDIR     = bin

# Build configuration stamp.
#
# Problem:
# This project uses build-time feature flags (e.g. ROCINANTE_TESTS,
# ROCINANTE_PAGING_BRINGUP). If you switch targets without cleaning,
# Make may incorrectly reuse object files built with a different set of
# preprocessor definitions, leading to link errors or (worse) subtly wrong
# kernels.
#
# Policy:
# Any change to the effective compiler/linker flags must trigger a rebuild of
# all objects.
FLAGS_STAMP = $(BINDIR)/.build-flags

TARGET     = $(BINDIR)/rocinante.elf

ASM_SRCDIR = $(SRCDIR)/asm
ASM_OBJDIR = $(BINDIR)/asm

SP_SRCDIR  = $(SRCDIR)/sp
SP_OBJDIR  = $(BINDIR)/sp

MEM_SRCDIR = $(SRCDIR)/memory
MEM_OBJDIR = $(BINDIR)/memory

TEST_SRCDIR = $(SRCDIR)/testing
TEST_OBJDIR = $(BINDIR)/testing

PLATFORM_SRCDIR = $(SRCDIR)/platform
PLATFORM_OBJDIR = $(BINDIR)/platform

BOOT_SRCDIR = $(SRCDIR)/boot
BOOT_OBJDIR = $(BINDIR)/boot

KERNEL_SRCDIR = $(SRCDIR)/kernel
KERNEL_OBJDIR = $(BINDIR)/kernel

TRAP_SRCDIR = $(SRCDIR)/trap
TRAP_OBJDIR = $(BINDIR)/trap

# Minimal C++ ABI/runtime stubs
ABI_SRC    = $(SRCDIR)/cxxabi.cpp
ABI_OBJ    = $(BINDIR)/cxxabi.o

ASM_SRCS   = $(wildcard $(ASM_SRCDIR)/*.S)
ASM_OBJS   = $(patsubst $(ASM_SRCDIR)/%.S, $(ASM_OBJDIR)/%.o, $(ASM_SRCS))

SP_SRCS    = $(wildcard $(SP_SRCDIR)/*.cpp)
SP_OBJS    = $(patsubst $(SP_SRCDIR)/%.cpp, $(SP_OBJDIR)/%.o, $(SP_SRCS))

MEM_SRCS   = $(wildcard $(MEM_SRCDIR)/*.cpp)
MEM_OBJS   = $(patsubst $(MEM_SRCDIR)/%.cpp, $(MEM_OBJDIR)/%.o, $(MEM_SRCS))

PLATFORM_SRCS = $(wildcard $(PLATFORM_SRCDIR)/*.cpp)
PLATFORM_OBJS = $(patsubst $(PLATFORM_SRCDIR)/%.cpp, $(PLATFORM_OBJDIR)/%.o, $(PLATFORM_SRCS))

BOOT_SRCS  = $(wildcard $(BOOT_SRCDIR)/*.cpp)
BOOT_OBJS  = $(patsubst $(BOOT_SRCDIR)/%.cpp, $(BOOT_OBJDIR)/%.o, $(BOOT_SRCS))

KERNEL_SRCS = $(wildcard $(KERNEL_SRCDIR)/*.cpp)
KERNEL_OBJS = $(patsubst $(KERNEL_SRCDIR)/%.cpp, $(KERNEL_OBJDIR)/%.o, $(KERNEL_SRCS))

TRAP_SRCS  = $(wildcard $(TRAP_SRCDIR)/*.cpp)
TRAP_OBJS  = $(patsubst $(TRAP_SRCDIR)/%.cpp, $(TRAP_OBJDIR)/%.o, $(TRAP_SRCS))

TEST_SRCS  = $(wildcard $(TEST_SRCDIR)/*.cpp)
TEST_OBJS  = $(patsubst $(TEST_SRCDIR)/%.cpp, $(TEST_OBJDIR)/%.o, $(TEST_SRCS))

ALL_OBJS   = $(ASM_OBJS) $(ABI_OBJ) $(SP_OBJS) $(MEM_OBJS) $(PLATFORM_OBJS) $(BOOT_OBJS) $(TRAP_OBJS) $(KERNEL_OBJS)

LINKER_LD  = $(ASM_SRCDIR)/linker.ld

## Toolchain and flags
## Cross-compilation to LoongArch64, compiler and linker flags
TARGET_TRIPLE ?= loongarch64-linux-gnu
CC             = clang
CXX            = clang++
LD             = $(CXX)
CFLAGS         = --target=$(TARGET_TRIPLE) -ffreestanding -nostdlib -fno-asynchronous-unwind-tables -fno-pic -fno-jump-tables -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize -Wall -Wextra -O2 -I$(PROJECT_ROOT_DIRECTORY)
CXXFLAGS       = $(CFLAGS) -fno-exceptions -fno-rtti -std=gnu++23
LDFLAGS        = --target=$(TARGET_TRIPLE) -fuse-ld=lld -nostdlib -static -Wl,-no-pie -Wl,-T,$(LINKER_LD) -Wl,-s


ifeq ($(ROCINANTE_TLBREFILL_UART_BREADCRUMBS),1)
	CFLAGS += -DROCINANTE_TLBREFILL_UART_BREADCRUMBS
	CXXFLAGS += -DROCINANTE_TLBREFILL_UART_BREADCRUMBS
endif

ifeq ($(ROCINANTE_TESTS),1)
	CXXFLAGS += -DROCINANTE_TESTS
	ALL_OBJS += $(TEST_OBJS)
endif

all: $(TARGET)

$(FLAGS_STAMP):
	@mkdir -p $(BINDIR)
	@tmpfile=$$(mktemp); \
	printf '%s\n' "CFLAGS=$(CFLAGS)" "CXXFLAGS=$(CXXFLAGS)" "LDFLAGS=$(LDFLAGS)" > "$$tmpfile"; \
	if [ ! -f "$@" ] || ! cmp -s "$$tmpfile" "$@"; then \
		mv "$$tmpfile" "$@"; \
	else \
		rm "$$tmpfile"; \
	fi

check-toolchain:
	@command -v $(CC) >/dev/null 2>&1 || (\
		echo "ERROR: Missing C compiler '$(CC)'"; \
		exit 1)
	@command -v $(CXX) >/dev/null 2>&1 || (\
		echo "ERROR: Missing C++ compiler '$(CXX)'"; \
		exit 1)
	@command -v ld.lld >/dev/null 2>&1 || (\
		echo "ERROR: Missing linker 'ld.lld' required for Clang cross-linking"; \
		echo "Install LLVM lld and re-run make."; \
		exit 1)

$(TARGET): check-toolchain $(FLAGS_STAMP) $(ALL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)

$(ASM_OBJDIR)/%.o: $(ASM_SRCDIR)/%.S $(FLAGS_STAMP)
	@mkdir -p $(ASM_OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SP_OBJDIR)/%.o: $(SP_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(SP_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(MEM_OBJDIR)/%.o: $(MEM_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(MEM_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PLATFORM_OBJDIR)/%.o: $(PLATFORM_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(PLATFORM_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BOOT_OBJDIR)/%.o: $(BOOT_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(BOOT_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TRAP_OBJDIR)/%.o: $(TRAP_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(TRAP_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL_OBJDIR)/%.o: $(KERNEL_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(KERNEL_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_OBJDIR)/%.o: $(TEST_SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(TEST_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(ABI_OBJ): $(ABI_SRC) $(FLAGS_STAMP)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@find $(BINDIR) -mindepth 1 -type f ! -name '.gitkeep' -delete
	@find $(BINDIR) -mindepth 1 -type d -empty -delete
	@echo "Cleaned build artifacts."

run: $(TARGET)
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-monitor none -kernel $<

run-serial: $(TARGET)
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-nographic -serial stdio -monitor none \
		-kernel $<

test:
	@$(MAKE) -C $(PROJECT_ROOT_DIRECTORY) ROCINANTE_TESTS=1 MAKEFLAGS= clean all
	@$(MAKE) -C $(PROJECT_ROOT_DIRECTORY) ROCINANTE_TESTS=1 MAKEFLAGS= run-serial

# Audit for absolute-address pointer tables.
#
# Context:
# During paging bring-up we intentionally run the kernel via a higher-half alias,
# and we can tear down the low identity mapping. Any data structure containing
# link-time absolute addresses (e.g. jump tables, vtables, string-pointer tables)
# can reintroduce unmapped low addresses and fault.
#
# Spec anchor:
# LoongArch-ELF-ABI-EN.html, Relocations table:
#   R_LARCH_32 / R_LARCH_64: *(int{32,64}_t*)PC = RtAddr + A
# These relocations are the mechanism by which absolute addresses can be written
# into object-file data.
audit-abs-ptrs:
	@command -v llvm-objdump >/dev/null 2>&1 || (\
		echo "ERROR: Missing 'llvm-objdump' (needed for audit-abs-ptrs)"; \
		exit 1)
	@command -v llvm-nm >/dev/null 2>&1 || (\
		echo "ERROR: Missing 'llvm-nm' (needed for audit-abs-ptrs)"; \
		exit 1)
	@fail=0; \
	for f in $(ALL_OBJS); do \
		if llvm-objdump -r "$$f" | grep -q "RELOCATION RECORDS FOR \[\.rodata"; then \
			echo "ERROR: $$f has relocations in .rodata (likely pointer table)"; \
			fail=1; \
		fi; \
		if llvm-objdump -r "$$f" | grep -q "RELOCATION RECORDS FOR \[\.data\.rel\.ro"; then \
			echo "ERROR: $$f has relocations in .data.rel.ro (likely vtable/typeinfo)"; \
			fail=1; \
		fi; \
		if llvm-nm --demangle "$$f" | grep -qE "vtable for|typeinfo for"; then \
			echo "ERROR: $$f defines vtable/typeinfo symbols"; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -eq 0 ]; then \
		echo "audit-abs-ptrs: OK"; \
	fi; \
	exit $$fail

.PHONY: all check-toolchain clean run run-serial test run-tests audit-abs-ptrs

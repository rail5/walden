# Makefile for building Rocinante kernel
PROJECT_ROOT_DIRECTORY = $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

## Directories and files
## Where to find sources, where to put objects, and what the final target is
SRCDIR     = src
BINDIR     = bin

TARGET     = $(BINDIR)/rocinante.elf

ASM_SRCDIR = $(SRCDIR)/asm
ASM_OBJDIR = $(BINDIR)/asm

SP_SRCDIR  = $(SRCDIR)/sp
SP_OBJDIR  = $(BINDIR)/sp

MEM_SRCDIR = $(SRCDIR)/memory
MEM_OBJDIR = $(BINDIR)/memory

TEST_SRCDIR = $(SRCDIR)/testing
TEST_OBJDIR = $(BINDIR)/testing

KERNEL_SRC = $(SRCDIR)/kernel.cpp
KERNEL_OBJ = $(BINDIR)/kernel.o

TRAP_SRC   = $(SRCDIR)/trap.cpp
TRAP_OBJ   = $(BINDIR)/trap.o

# Minimal C++ ABI/runtime stubs
ABI_SRC    = $(SRCDIR)/cxxabi.cpp
ABI_OBJ    = $(BINDIR)/cxxabi.o

ASM_SRCS   = $(wildcard $(ASM_SRCDIR)/*.S)
ASM_OBJS   = $(patsubst $(ASM_SRCDIR)/%.S, $(ASM_OBJDIR)/%.o, $(ASM_SRCS))

SP_SRCS    = $(wildcard $(SP_SRCDIR)/*.cpp)
SP_OBJS    = $(patsubst $(SP_SRCDIR)/%.cpp, $(SP_OBJDIR)/%.o, $(SP_SRCS))

MEM_SRCS   = $(wildcard $(MEM_SRCDIR)/*.cpp)
MEM_OBJS   = $(patsubst $(MEM_SRCDIR)/%.cpp, $(MEM_OBJDIR)/%.o, $(MEM_SRCS))

TEST_SRCS  = $(wildcard $(TEST_SRCDIR)/*.cpp)
TEST_OBJS  = $(patsubst $(TEST_SRCDIR)/%.cpp, $(TEST_OBJDIR)/%.o, $(TEST_SRCS))

ALL_OBJS   = $(ASM_OBJS) $(KERNEL_OBJ) $(TRAP_OBJ) $(ABI_OBJ) $(SP_OBJS) $(MEM_OBJS)

LINKER_LD  = $(ASM_SRCDIR)/linker.ld

## Toolchain and flags
## Cross-compilation to LoongArch64, compiler and linker flags
TARGET_TRIPLE ?= loongarch64-linux-gnu
CC             = clang
CXX            = clang++
LD             = $(CXX)
CFLAGS         = --target=$(TARGET_TRIPLE) -ffreestanding -nostdlib -fno-asynchronous-unwind-tables -fno-pic -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize -Wall -Wextra -O2 -I$(PROJECT_ROOT_DIRECTORY)
CXXFLAGS       = $(CFLAGS) -fno-exceptions -fno-rtti -std=gnu++23
LDFLAGS        = --target=$(TARGET_TRIPLE) -fuse-ld=lld -nostdlib -static -Wl,-no-pie -Wl,-T,$(LINKER_LD) -Wl,-s

ifeq ($(ROCINANTE_PAGING_BRINGUP),1)
	CFLAGS += -DROCINANTE_PAGING_BRINGUP
	CXXFLAGS += -DROCINANTE_PAGING_BRINGUP
endif

ifeq ($(ROCINANTE_TESTS),1)
	CXXFLAGS += -DROCINANTE_TESTS
	ALL_OBJS += $(TEST_OBJS)
endif

all: $(TARGET)

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

$(TARGET): check-toolchain $(ALL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)

$(ASM_OBJDIR)/%.o: $(ASM_SRCDIR)/%.S
	@mkdir -p $(ASM_OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SP_OBJDIR)/%.o: $(SP_SRCDIR)/%.cpp
	@mkdir -p $(SP_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(MEM_OBJDIR)/%.o: $(MEM_SRCDIR)/%.cpp
	@mkdir -p $(MEM_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_OBJDIR)/%.o: $(TEST_SRCDIR)/%.cpp
	@mkdir -p $(TEST_OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL_OBJ): $(KERNEL_SRC)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TRAP_OBJ): $(TRAP_SRC)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(ABI_OBJ): $(ABI_SRC)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@find $(BINDIR) -mindepth 1 -type f ! -name '.gitkeep' -delete
	@find $(BINDIR) -mindepth 1 -type d -empty -delete
	@echo "Cleaned build artifacts."

run: $(TARGET)
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-monitor none -kernel $< -append "test=1"

run-serial: $(TARGET)
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-nographic -serial stdio -monitor none \
		-kernel $< -append "test=1"

test:
	@$(MAKE) -C $(PROJECT_ROOT_DIRECTORY) ROCINANTE_TESTS=1 MAKEFLAGS= clean all
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-nographic -serial stdio -monitor none \
		-kernel $(PROJECT_ROOT_DIRECTORY)/$(TARGET)

.PHONY: all check-toolchain clean run run-serial test run-tests

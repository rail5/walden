## Directories and files
## Where to find sources, where to put objects, and what the final target is
SRCDIR          := src
BINDIR          := bin
TARGET          := $(BINDIR)/rocinante.elf
ASM_SRCDIR      := $(SRCDIR)/asm
ASM_OBJDIR      := $(BINDIR)/asm
SP_SRCDIR       := $(SRCDIR)/sp
SP_OBJDIR       := $(BINDIR)/sp
MEM_SRCDIR      := $(SRCDIR)/memory
MEM_OBJDIR      := $(BINDIR)/memory
TEST_SRCDIR     := $(SRCDIR)/testing
TEST_OBJDIR     := $(BINDIR)/testing
PLATFORM_SRCDIR := $(SRCDIR)/platform
PLATFORM_OBJDIR := $(BINDIR)/platform
BOOT_SRCDIR     := $(SRCDIR)/boot
BOOT_OBJDIR     := $(BINDIR)/boot
KERNEL_SRCDIR   := $(SRCDIR)/kernel
KERNEL_OBJDIR   := $(BINDIR)/kernel
TRAP_SRCDIR     := $(SRCDIR)/trap
TRAP_OBJDIR     := $(BINDIR)/trap
# Minimal C++ ABI/runtime stubs
ABI_SRC         := $(SRCDIR)/cxxabi.cpp
ABI_OBJ         := $(BINDIR)/cxxabi.o
LINKER_LD       := $(ASM_SRCDIR)/linker.ld

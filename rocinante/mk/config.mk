# Toolchain and flags
## Cross-compilation to LoongArch64, compiler and linker flags
TARGET_TRIPLE ?= loongarch64-linux-gnu
CC            := clang
CXX           := clang++
LD            := $(CXX)
CFLAGS        := --target=$(TARGET_TRIPLE) -ffreestanding -nostdlib -fno-asynchronous-unwind-tables -fno-pic -fno-jump-tables -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize -Wall -Wextra -O2 -I$(PROJECT_ROOT_DIRECTORY)
CXXFLAGS      := $(CFLAGS) -fno-exceptions -fno-rtti -std=gnu++23
LDFLAGS       := --target=$(TARGET_TRIPLE) -fuse-ld=lld -nostdlib -static -Wl,-no-pie -Wl,-T,$(LINKER_LD) -Wl,-s


ifeq ($(ROCINANTE_TLBREFILL_UART_BREADCRUMBS),1)
	CFLAGS += -DROCINANTE_TLBREFILL_UART_BREADCRUMBS
	CXXFLAGS += -DROCINANTE_TLBREFILL_UART_BREADCRUMBS
endif

ifeq ($(ROCINANTE_TESTS),1)
	CXXFLAGS += -DROCINANTE_TESTS
	ALL_OBJS += $(TEST_OBJS)
endif

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

$(FLAGS_STAMP):
	@mkdir -p $(BINDIR)
	@tmpfile=$$(mktemp); \
	printf '%s\n' "CFLAGS=$(CFLAGS)" "CXXFLAGS=$(CXXFLAGS)" "LDFLAGS=$(LDFLAGS)" > "$$tmpfile"; \
	if [ ! -f "$@" ] || ! cmp -s "$$tmpfile" "$@"; then \
		mv "$$tmpfile" "$@"; \
	else \
		rm "$$tmpfile"; \
	fi

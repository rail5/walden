# Generate a compilation database for clangd/clang-tidy.
# Requires: `bear`
compdb:
	@command -v bear >/dev/null 2>&1 || (\
		echo "ERROR: Missing 'bear'. Install it (e.g. apt install bear)"; \
		exit 1)
	@rm -f $(PROJECT_ROOT_DIRECTORY)/../compile_commands.json
	@bear --output $(PROJECT_ROOT_DIRECTORY)/../compile_commands.json -- $(MAKE) clean all

run: $(TARGET)
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-monitor none -kernel $<

run-serial: $(TARGET)
	qemu-system-loongarch64 -machine virt -cpu la464 -m 256M -smp 1 \
		-nographic -serial stdio -monitor none \
		-kernel $<

test:
	@$(MAKE) -C $(PROJECT_ROOT_DIRECTORY) ROCINANTE_TESTS=1 MAKEFLAGS= clean all run-serial

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

.PHONY: all check-toolchain clean run run-serial test run-tests audit-abs-ptrs compdb

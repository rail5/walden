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

# Tests are organized in nested subdirectories. Mirror the source layout into
# the object directory.
$(BINDIR)/%.o: $(SRCDIR)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/%.o: $(SRCDIR)/%.S $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

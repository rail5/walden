all: kernel

kernel:
	@echo "Building the Rocinante kernel..."
	@$(MAKE) -C rocinante

run:
	@echo "Running Walden in QEMU..."
	@$(MAKE) -C rocinante run

clean:
	@echo "Cleaning build artifacts..."
	@$(MAKE) -C rocinante clean

compdb:
	@echo "Generating compile_commands.json (requires 'bear')..."
	@$(MAKE) -C rocinante compdb

.PHONY: all kernel run clean compdb

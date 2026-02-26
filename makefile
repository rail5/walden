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

.PHONY: all kernel run clean

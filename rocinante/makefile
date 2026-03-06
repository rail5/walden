# Makefile for building Rocinante kernel
PROJECT_ROOT_DIRECTORY := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

all: bin/rocinante.elf

clean:
	@find $(BINDIR) -mindepth 1 -type f ! -name '.gitkeep' -delete
	@find $(BINDIR) -mindepth 1 -type d -empty -delete
	@echo "Cleaned build artifacts."

# Split build logic into focused include files.
include $(PROJECT_ROOT_DIRECTORY)/mk/paths.mk
include $(PROJECT_ROOT_DIRECTORY)/mk/sources.mk
include $(PROJECT_ROOT_DIRECTORY)/mk/config.mk
include $(PROJECT_ROOT_DIRECTORY)/mk/rules.mk
include $(PROJECT_ROOT_DIRECTORY)/mk/targets.mk

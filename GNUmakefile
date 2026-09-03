# MintSCAN top-level GNU make wrapper.
#
# Makefile is the build and packaging source of truth. The wrapper delegates
# the build and only fixes the packaged drawer's saved Workbench window height.
# Source .info files are never modified by this post-processing step.

FORWARD_TARGETS := help check test-http test-mdns check-art

.PHONY: all release clean $(FORWARD_TARGETS)

all:
	$(MAKE) -f Makefile all

$(FORWARD_TARGETS):
	$(MAKE) -f Makefile $@

release:
	$(MAKE) -f Makefile release
	@# Classic Amiga drawer icons store the NewWindow height as a big-endian
	@# WORD at byte offset 84 (DiskObject header 78 + 6 bytes into DrawerData).
	@if [ -f release/MintSCAN.info ]; then \
		printf '\000\170' | dd of=release/MintSCAN.info bs=1 seek=84 conv=notrunc 2>/dev/null; \
		echo "Set packaged drawer window height to 120: release/MintSCAN.info"; \
	fi

clean:
	$(MAKE) -f Makefile clean

%:
	$(MAKE) -f Makefile $@

# MintSCAN top-level GNU make wrapper.
#
# Makefile is the build and packaging source of truth. The wrapper only fixes
# the packaged drawer's saved Workbench window height after check-art has
# verified that MintSCAN.info really is a WBDRAWER DiskObject.

FORWARD_TARGETS := help check test-http test-mdns check-art

.PHONY: all release clean $(FORWARD_TARGETS)

all:
	$(MAKE) -f Makefile all

$(FORWARD_TARGETS):
	$(MAKE) -f Makefile $@

release:
	$(MAKE) -f Makefile release
	@printf '\\000\\170' | dd of=release/MintSCAN.info bs=1 seek=84 conv=notrunc 2>/dev/null
	@echo "Set packaged drawer window height to 120: release/MintSCAN.info"

clean:
	$(MAKE) -f Makefile clean

%:
	$(MAKE) -f Makefile $@

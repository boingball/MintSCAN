# MintSCAN top-level wrapper.
#
# Keep the existing Makefile as the build source of truth. This GNUmakefile
# delegates normal targets to it and only post-processes the packaged drawer
# icon so its saved Workbench window is not the tiny default geometry in the
# source artwork - same fix MintPRINT applies to its own release drawers.
# Source .info files are never modified.

FORWARD_TARGETS := help

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
		echo "Set drawer window height to 120: release/MintSCAN.info"; \
	fi

clean:
	$(MAKE) -f Makefile clean

# Keep arbitrary/internal Makefile targets reachable too. Public command
# targets above are explicit so directory-name collisions cannot intercept
# them.
%:
	$(MAKE) -f Makefile $@

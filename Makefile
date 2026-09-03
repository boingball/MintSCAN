CROSS      ?= m68k-amigaos-
CC          = $(CROSS)gcc
HOST_CC    ?= cc
CFLAGS     ?= -O2 -m68000 -Wall -Wextra -fomit-frame-pointer -fno-builtin
HOST_CFLAGS ?= -O2 -Wall -Wextra -pedantic

BUILD_DIR  := build
TEST_DIR   := $(BUILD_DIR)/tests
ART_DIR    := art
RELEASE_DIR := release/MintSCAN

APP_ICON     := $(ART_DIR)/MintScan.info
DRAWER_ICON  := $(ART_DIR)/MintSCAN.info
INSTALL_ICON := $(ART_DIR)/Install.info

.PHONY: all help check test-http test-mdns check-art release clean

all: MintScan

help:
	@echo "MintSCAN targets:"
	@echo "  make           - build MintScan for m68k AmigaOS"
	@echo "  make check     - run host-side HTTP and DNS-SD tests"
	@echo "  make release   - build, validate art and stage the Aminet bundle"
	@echo "  make clean"

MintScan: src/MintScan.c src/http_response.c src/http_response.h src/mdns_endpoint.c src/mdns_endpoint.h
	$(CC) $(CFLAGS) -Isrc -o $@ src/MintScan.c src/http_response.c src/mdns_endpoint.c -lamiga -lm

$(TEST_DIR):
	mkdir -p $(TEST_DIR)

test-http: | $(TEST_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc -o $(TEST_DIR)/test_http_response tests/test_http_response.c src/http_response.c
	$(TEST_DIR)/test_http_response

test-mdns: | $(TEST_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc -o $(TEST_DIR)/test_mdns_endpoint tests/test_mdns_endpoint.c src/mdns_endpoint.c
	$(TEST_DIR)/test_mdns_endpoint

check: test-http test-mdns

# The release icons are real Amiga DiskObjects, not PNGs. Their type byte is
# checked here so a tool icon can never be shipped as a drawer or installer
# icon. The app stack is kept in sync with src/MintScan.c ($STACK:131072).
check-art:
	@test -f $(APP_ICON) || { echo "Missing $(APP_ICON)"; exit 1; }
	@test -f $(DRAWER_ICON) || { echo "Missing $(DRAWER_ICON)"; exit 1; }
	@test -f $(INSTALL_ICON) || { echo "Missing $(INSTALL_ICON)"; exit 1; }
	@set -e; \
	app_type=$$(dd if=$(APP_ICON) bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' \\n'); \
	drawer_type=$$(dd if=$(DRAWER_ICON) bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' \\n'); \
	install_type=$$(dd if=$(INSTALL_ICON) bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' \\n'); \
	app_stack=$$(dd if=$(APP_ICON) bs=1 skip=74 count=4 2>/dev/null | od -An -tx1 | tr -d ' \\n'); \
	test "$$app_type" = "3" || { echo "ERROR: $(APP_ICON) must be WBTOOL (type 3), got $$app_type"; exit 1; }; \
	test "$$drawer_type" = "2" || { echo "ERROR: $(DRAWER_ICON) must be WBDRAWER (type 2), got $$drawer_type"; exit 1; }; \
	test "$$install_type" = "4" || { echo "ERROR: $(INSTALL_ICON) must be WBPROJECT (type 4), got $$install_type"; exit 1; }; \
	test "$$app_stack" = "00020000" || { echo "ERROR: $(APP_ICON) Stack must be 131072 bytes, got 0x$$app_stack"; exit 1; }
	@if cmp -s $(APP_ICON) $(DRAWER_ICON); then \
		echo "ERROR: application and drawer icons are byte-identical"; \
		exit 1; \
	fi
	@echo "Artwork types and MintScan stack are valid"

release: check MintScan check-art
	rm -rf release
	mkdir -p $(RELEASE_DIR)
	cp MintScan $(RELEASE_DIR)/MintScan
	cp $(APP_ICON) $(RELEASE_DIR)/MintScan.info
	cp docs/MintSCAN.guide $(RELEASE_DIR)/
	cp LICENSE $(RELEASE_DIR)/
	cp Install release/Install
	cp $(INSTALL_ICON) release/Install.info
	cp $(DRAWER_ICON) release/MintSCAN.info
	cp Aminet/MintSCAN.readme release/MintSCAN.readme
	@echo
	@echo "MintSCAN 1.1.0 release staged under release/:"
	@echo "  Install / Install.info"
	@echo "  MintSCAN.info"
	@echo "  MintSCAN.readme"
	@echo "  MintSCAN/MintScan and MintScan.info"
	@echo "  MintSCAN/MintSCAN.guide"
	@echo "  MintSCAN/LICENSE"

clean:
	rm -rf build release MintScan

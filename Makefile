CROSS      ?= m68k-amigaos-
CC          = $(CROSS)gcc
HOST_CC    ?= cc
CFLAGS     ?= -O2 -m68000 -Wall -Wextra -fomit-frame-pointer -fno-builtin
HOST_CFLAGS ?= -O2 -Wall -Wextra -pedantic

BUILD_DIR  := build
TEST_DIR   := $(BUILD_DIR)/tests
ART_DIR    := art
RELEASE_DIR := release/MintSCAN

.PHONY: all help check test-http test-mdns check-art release clean

all: MintScan

help:
	@echo "MintSCAN targets:"
	@echo "  make           - build MintScan for m68k AmigaOS"
	@echo "  make check     - run host-side HTTP and DNS-SD tests"
	@echo "  make release   - validate artwork and stage the Aminet bundle"
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

# Artwork is intentionally a hard release gate. The old repository files used
# the same WBTOOL DiskObject as both application and drawer icons, which makes
# the drawer invalid and causes the GNUmakefile geometry patch to modify the
# wrong structure. See art/README.md before replacing the placeholders.
check-art:
	@test -f $(ART_DIR)/MintScan.info || { echo "Missing $(ART_DIR)/MintScan.info"; exit 1; }
	@test -f $(ART_DIR)/MintSCAN.info || { echo "Missing $(ART_DIR)/MintSCAN.info"; exit 1; }
	@test -f $(ART_DIR)/Install.info || { echo "Missing $(ART_DIR)/Install.info"; exit 1; }
	@if cmp -s $(ART_DIR)/MintScan.info $(ART_DIR)/MintSCAN.info; then \
		echo "ERROR: application and drawer icons are identical placeholders"; \
		exit 1; \
	fi
	@set -e; \
	app_type=$$(dd if=$(ART_DIR)/MintScan.info bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' \n'); \
	drawer_type=$$(dd if=$(ART_DIR)/MintSCAN.info bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' \n'); \
	install_type=$$(dd if=$(ART_DIR)/Install.info bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' \n'); \
	app_stack=$$(dd if=$(ART_DIR)/MintScan.info bs=1 skip=74 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n'); \
	test "$$app_type" = "3" || { echo "ERROR: MintScan.info must be WBTOOL (type 3), got $$app_type"; exit 1; }; \
	test "$$drawer_type" = "2" || { echo "ERROR: MintSCAN.info must be WBDRAWER (type 2), got $$drawer_type"; exit 1; }; \
	test "$$install_type" = "4" || { echo "ERROR: Install.info must be WBPROJECT (type 4), got $$install_type"; exit 1; }; \
	test "$$app_stack" = "00020000" || { echo "ERROR: MintScan.info Stack must be 131072 bytes, got 0x$$app_stack"; exit 1; }
	@echo "Artwork types and MintScan stack are valid"

release: check MintScan check-art
	rm -rf release
	mkdir -p $(RELEASE_DIR)
	cp MintScan $(RELEASE_DIR)/
	cp $(ART_DIR)/MintScan.info $(RELEASE_DIR)/
	cp docs/MintSCAN.guide $(RELEASE_DIR)/
	cp LICENSE $(RELEASE_DIR)/
	cp Install release/
	cp $(ART_DIR)/Install.info release/
	cp $(ART_DIR)/MintSCAN.info release/
	cp Aminet/MintSCAN.readme release/
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

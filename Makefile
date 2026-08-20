CROSS   ?= m68k-amigaos-
CC       = $(CROSS)gcc
CFLAGS  ?= -O2 -m68000 -Wall -Wextra -fomit-frame-pointer -fno-builtin

.PHONY: all release clean help

all: MintScan

help:
	@echo "MintSCAN targets:"
	@echo "  make          - build MintScan"
	@echo "  make release  - stage a distributable bundle in release/MintSCAN/"
	@echo "  make clean"

MintScan: src/MintScan.c
	$(CC) $(CFLAGS) -o $@ src/MintScan.c -lamiga -lm

ART_DIR := art
RELEASE_DIR := release/MintSCAN

# Mirrors MintPRINT's release layout: icons copied from art/ if present,
# with the drawer's own icon staged in the parent of RELEASE_DIR per
# AmigaOS convention (a drawer's icon lives next to the drawer, not
# inside it).
release: MintScan
	mkdir -p $(RELEASE_DIR)
	cp MintScan $(RELEASE_DIR)/
	@if [ -f $(ART_DIR)/MintScan.info ]; then \
		cp $(ART_DIR)/MintScan.info $(RELEASE_DIR)/; \
		echo "Copied $(ART_DIR)/MintScan.info -> $(RELEASE_DIR)/"; \
	else \
		echo "No $(ART_DIR)/MintScan.info found - application will have no icon"; \
	fi
	@if [ -f $(ART_DIR)/MintSCAN.info ]; then \
		cp $(ART_DIR)/MintSCAN.info release/MintSCAN.info; \
		echo "Copied $(ART_DIR)/MintSCAN.info -> release/MintSCAN.info (drawer icon)"; \
	else \
		echo "No $(ART_DIR)/MintSCAN.info found - release drawer will have no icon"; \
	fi
	@echo
	@echo "Release bundle staged in $(RELEASE_DIR)/"

clean:
	rm -rf build release MintScan

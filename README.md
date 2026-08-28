<h1>
  <img src="art/MintScan.png" width="48" alt="MintSCAN icon">
  MintSCAN
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-orange)
![CPU](https://img.shields.io/badge/CPU-m68k-blue)
![Scanning](https://img.shields.io/badge/Scanning-eSCL%20%2F%20AirScan-0078D4)
![Formats](https://img.shields.io/badge/Formats-JPEG%20%7C%20PNG%20%7C%20PDF-purple)
![Discovery](https://img.shields.io/badge/Discovery-mDNS-green)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

[![Aminet](https://img.shields.io/badge/Download-Aminet-005CA9)](https://aminet.net/package/gfx/misc/MintSCAN)
[![Support](https://img.shields.io/badge/Support-Buy%20Me%20a%20Coffee-FFDD00?logo=buymeacoffee&logoColor=000000)](https://buymeacoffee.com/boingball)

![GitHub stars](https://img.shields.io/github/stars/boingball/MintSCAN)
![GitHub last commit](https://img.shields.io/github/last-commit/boingball/MintSCAN)

**Modern network scanning for classic Amigas.**

Scan directly to a file from normal Amiga Workbench using modern eSCL
(AirScan/AirPrint Scan) network scanners and all-in-one printers - no
PC scan server required.

# MintSCAN

eSCL (AirScan/AirPrint Scan) scanning for classic AmigaOS - scan from
modern network scanners/MFPs straight to a file on your Amiga, no
scanner-specific software required on the device side.

Sibling project to [MintPRINT](https://github.com/boingball/MintPRINT)
(IPP/AirPrint printing for classic AmigaOS) - built the same way, reusing
the same bsdsocket networking, mDNS discovery and GadTools idioms, but as
a standalone application rather than a device driver. Scanning has no
AmigaOS "device" concept to hook into the way printing has
`DEVS:Printers/`, so there is no driver component here - just one GUI
program.

## What's new in 1.2.1

- **Grey the Scan button out while a scan is running**, re-enabling once
  it returns.
- **Fixed a build error** in the new "Browse" file requester code
  (`FilePart()` returns `STRPTR`/`unsigned char *`; `savepath_buffer` is
  `char *` - GCC won't subtract pointers to different base types even
  though both are single-byte) - caught on a real `m68k-amigaos-gcc`
  build.
- **Black & White no longer produces a badly squashed, garbled scan -
  confirmed scanner-side, not an Amiga or MintSCAN problem.** Two wrong
  guesses along the way (a JPEG container limit, then an Amiga
  picture-datatype limitation) before actually looking at what
  `windows_escl_probe.py <ip> --color BlackAndWhite1` saved settled it:
  full RGB colour, with visible horizontal banding, despite explicitly
  requesting `BlackAndWhite1` - fetched by a plain PC script with no
  Amiga anywhere in the path. The scanner just isn't honouring that
  `ColorMode` value. `build_scan_settings_xml()` now substitutes
  Grayscale for `BlackAndWhite1` across every format and says so - the
  same scanner does honour Grayscale8 correctly. See
  `docs/ARCHITECTURE.md` for the full trail.

## What's new in 1.2.0

MintSCAN 1.2.0 fixes a build-breaking typo caught on a real
`m68k-amigaos-gcc` build, then follows up on the first real-hardware
screenshot of 1.1.0 with a GUI pass and a file requester.

- **Fixed a pre-existing comment bug that broke the real cross-compiler
  build.** A doc comment above `load_unit_config()` read
  `scanner_*/source_index` - the literal `*/` closed the block comment
  early, so everything from there through the function body was parsed
  as stray C tokens instead of a comment. That's what caused the syntax
  error plus the "implicit declaration of `load_unit_config`" and
  "defined but not used" warnings for `reset_unit_defaults`/
  `find_label_index` (all three are only reachable from inside
  `load_unit_config`, whose body the parser never actually saw).
- **"Browse" file requester on Save to.** `do_browse_savepath()` opens a
  standard ASL file requester (pre-seeded from the current path via
  `FilePart()`/`AddPart()`) instead of requiring a hand-typed AmigaDOS
  path. Optional at runtime - if `asl.library` doesn't open, the button
  just says so rather than the app refusing to start.
- **Model and Status are plain text now, not greyed-out fields.** Both
  were disabled `STRING_KIND` gadgets, which this render as a
  hatched/dimmed box reading as "unavailable" - switched to `TEXT_KIND`
  (matching MintPRINT's own Printer Model display).
- **Status now shows on load**, not just after the first Discover/Query -
  a saved Unit's scanner gets queried once the window opens.
- **Scanner dropdown no longer overflows.** Its label included the model
  name ("192.168.0.71 (Brother MFC-J6930DW)"), long enough to run behind
  the Discover button - dropped, since Model right below it already
  shows the same name.
- **DPI/Format/Size moved into a second column** beside Source/Colour,
  balancing the window's width instead of stacking five dropdowns above
  a mostly-empty right half - shortens the form by two rows, most of
  which went into tightening the gap above the status box (was noticeably
  loose in the 1.1.0 screenshot).

## What's new in 1.1.0

MintSCAN 1.1.0 closes the one remaining gap from the MintPRINT-learnings
review: `send()` had no timeout even though `connect()`/`recv()` already
did, plus a real eSCL capability (`ScannerStatus`) that wasn't used yet,
plus a GUI pass to match MintPRINT's conventions more closely.

- **No more indefinite hangs on a scanner that stops draining its TCP
  receive window mid-request.** `connect()` and `recv()` were already
  timeout-bounded, but every `send()` call (headers, the ScanSettings XML
  body, `DELETE` cleanup, `NextDocument`'s request) was still a plain
  blocking call - the same gap MintPRINT's `ipp_client.c` had before its
  1.2.5 fix. `send_timeout()` (`src/MintScan.c`) mirrors the existing
  `recv_timeout()` - `WaitSelect()`-bounded, looped for short writes -
  and is now used everywhere `send()` was.
- **Live scanner status.** `/eSCL/ScannerStatus` was discovered but never
  queried. `query_scanner_status()` now reads its `pwg:State`
  (Idle/Processing/Testing/Stopped/Down) after every successful
  capabilities query and shows it in a new **Status:** field next to
  Model - the same idea as MintPRINT showing live `printer-state` next to
  its ink/toner strip. A failed `ScanJobs` also queries it, so "status
  503" becomes "ScanJobs failed (status 503) - scanner reports:
  Processing" instead of a bare code.
- **"IPv4:" instead of "IP:".** The manual-entry field only ever reaches
  `inet_addr()` (see `http_connect_once()`), so a hostname was never
  actually supported - same accurate-labeling fix MintPRINT applied to
  its own IP field once it noticed the same thing.
- Window grew by 20px to fit the new Status row; everything below it
  shifted down to match.

## What's new in 1.0.0

- **Saved scanner profiles, Unit0-7.** Same idea as MintPRINT's printer
  switcher: pick from up to eight saved profiles with the `Unit:`
  dropdown. There's no separate "activate" step - whichever Unit is
  loaded is what `Scan` uses next.
- **Wi-Fi scanners that sleep between jobs now get a fair chance to wake
  up.** `http_connect()` bounds every connection attempt with a
  non-blocking, `WaitSelect()`-polled connect and retries once on
  failure - the same fix MintPRINT shipped for Wi-Fi printers that drop
  their radio into power-save between jobs.
- **Status box survives window refresh**, and Discover/Query connections
  are cleanly timeout-bounded end to end - see `docs/ARCHITECTURE.md`.

## What's here

- **`src/MintScan.c`** - the GUI app. Discovers eSCL scanners on the LAN
  (mDNS/`_uscan._tcp`), queries `ScannerCapabilities`, lets you pick
  source/colour mode/resolution/format/page size, and scans straight to
  a file via the eSCL `ScanJobs` / `NextDocument` flow. See
  `docs/ARCHITECTURE.md` for how the eSCL side of this works, including
  the Unit0-7 saved-profile switcher and the Wi-Fi wake-up retry.
- **`windows_escl_probe.py`** - a Windows/any-PC-runnable diagnostic
  script for isolating scanner-side vs Amiga-side issues without
  needing Amiga tooling in the loop - same idea as MintPRINT's
  `windows_ipp_probe.py`. Fetches `ScannerCapabilities`, POSTs the same
  `ScanSettings` body MintScan.c would build, and downloads
  `NextDocument`, printing raw status/headers at every step.
- **`docs/`** - design notes, written as the project goes.
- **`Aminet/`** - the Aminet package description (`MintSCAN.readme`).

## Building

Requires `m68k-amigaos-gcc` (Bebbo's cross-toolchain) on `PATH`, or set
`CROSS=` to a different prefix.

    make          # builds MintScan
    make release  # stages a distributable bundle in release/MintSCAN/
    make clean

`make release` copies `MintScan.info`/`MintSCAN.info` in from `art/` if
present, and (via the top-level `GNUmakefile`) patches the packaged
drawer icon's saved Workbench window height so it doesn't open at the
tiny default size baked into the source artwork - same fix MintPRINT
applies to its own release drawers.

## Diagnosing a scanner that won't cooperate

If a scan comes back at the wrong resolution/colour mode, or the file
won't open, run `windows_escl_probe.py` from a PC on the same network
as the scanner (no Amiga/WinUAE needed) and compare its output against
what the status box in MintScan showed:

    python windows_escl_probe.py <scanner-ip> --dpi 400 --color BlackAndWhite1

It saves the raw `ScannerCapabilities` response to `escl_capabilities.xml`,
prints the exact `ScanSettings` XML it POSTs, and reports the actual
image it gets back - useful for telling apart "the scanner is ignoring
the request" from "the request itself is still wrong."

## Reporting a problem

If a scan comes back wrong or fails outright, please
[open an issue](https://github.com/boingball/MintSCAN/issues) and attach
the output of `windows_escl_probe.py` run against your scanner from a PC
on the same network, plus your AmigaOS version, TCP/IP stack, and the
exact Source/Colour/DPI/Format/Size options used.

## Status

Discovery, capability query, and a single-page scan-to-file flow are
working end to end against real hardware (a Brother MFC-J6930DW), with
saved multi-scanner profiles (Unit0-7) and a timeout-bounded, retried
connection for scanners that sleep their Wi-Fi radio between jobs.
See `docs/ARCHITECTURE.md` for open issues and what's still missing
(ADF/duplex, HTTPS/AmiSSL, full capabilities parsing).

## License

[MIT](LICENSE) - Copyright (c) 2026 Darren Banfi (boingball).

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

## What's here

- **`src/MintScan.c`** - the GUI app. Discovers eSCL scanners on the LAN
  (mDNS/`_uscan._tcp`), queries `ScannerCapabilities`, lets you pick
  source/colour mode/resolution/format/page size, and scans straight to
  a file via the eSCL `ScanJobs` / `NextDocument` flow. See
  `docs/ARCHITECTURE.md` for how the eSCL side of this works.
- **`windows_escl_probe.py`** - a Windows/any-PC-runnable diagnostic
  script for isolating scanner-side vs Amiga-side issues without
  needing Amiga tooling in the loop - same idea as MintPRINT's
  `windows_ipp_probe.py`. Fetches `ScannerCapabilities`, POSTs the same
  `ScanSettings` body MintScan.c would build, and downloads
  `NextDocument`, printing raw status/headers at every step.
- **`docs/`** - design notes, written as the project goes.

## Building

Requires `m68k-amigaos-gcc` (Bebbo's cross-toolchain) on `PATH`, or set
`CROSS=` to a different prefix.

    make          # builds MintScan
    make clean

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

## Status

Discovery, capability query, and a single-page scan-to-file flow are
working end to end against real hardware (a Brother MFC-J6930DW).
See `docs/ARCHITECTURE.md` for open issues and what's still missing
(ADF/duplex, HTTPS/AmiSSL, multi-scanner profiles, full capabilities
parsing).

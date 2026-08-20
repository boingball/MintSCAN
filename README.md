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
- **`docs/`** - design notes, written as the project goes.

## Building

Requires `m68k-amigaos-gcc` (Bebbo's cross-toolchain) on `PATH`, or set
`CROSS=` to a different prefix.

    make          # builds MintScan
    make clean

## Status

Early scaffold - discovery, capability query and a basic single-page
scan-to-file flow are in place. Not yet tested against real hardware.
See `docs/ARCHITECTURE.md` for open issues and what's still missing
(ADF/duplex, ScannerStatus polling, chunked-transfer NextDocument
responses, ScanRegions from a live preview).

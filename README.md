<h1>
  <img src="art/MintScan.png" width="48" alt="MintSCAN icon">
  MintSCAN
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-orange)
![CPU](https://img.shields.io/badge/CPU-m68k-blue)
![Scanning](https://img.shields.io/badge/Scanning-eSCL%20%2F%20AirScan-0078D4)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

**Modern network scanning for classic Amigas.**

MintSCAN is a standalone GadTools application which scans directly from an
eSCL/AirScan network scanner or multifunction printer to a file on your
Amiga. It is the scanning companion to
[MintPRINT](https://github.com/boingball/MintPRINT): no PC scan server,
vendor-specific Amiga driver, or system modification is required.

## Version 1.1.0

The 1.1.0 release adds and hardens:

- Eight independent saved scanner profiles (Unit0 to Unit7).
- Live scanner state, manual IPv4/port Query, and an ASL save requester.
- Timeout-bounded connect, send and receive operations, including one retry
  for sleeping Wi-Fi scanners.
- DNS-SD SRV port and TXT `rs=` resource-root support.
- Complete HTTP framing and disk-write checks, with partial scans removed
  after truncation, timeout, malformed chunking, or write failure.
- Source-scoped DPI/colour validation and a model-specific workaround for the
  Brother MFC-J6930DW `BlackAndWhite1` firmware fault.
- A 400 DPI choice, Installer script, AmigaGuide, portable parser tests, CI,
  and guarded release packaging.

See [CHANGELOG.md](CHANGELOG.md) for the release history.

## Requirements

- Motorola 68000 or later.
- AmigaOS 3.0 or later.
- A TCP/IP stack providing `bsdsocket.library`.
- An eSCL/AirScan scanner reachable over plain HTTP.
- A 128 KiB stack (set in both the program and supplied Workbench icon).

HTTPS-only `_uscans._tcp` devices are not supported yet because the client
does not use AmiSSL.

## Quick start

1. Start the Amiga TCP/IP stack and run `MintScan`.
2. Click **Discover**. If mDNS cannot cross the network or emulator, enter an
   IPv4 address (optionally `:port`) and click **Query**.
3. Check the reported Model and Status.
4. Choose Source, Colour, DPI, Format, Size, and a destination.
5. Click **Scan**.
6. Click **Save Config** to retain the settings in the selected Unit.

The scanner creates the JPEG, PNG, or PDF data; MintSCAN streams it unchanged
to disk. A successful scan is only reported after the HTTP transfer and file
close have completed.

The full user guide is [docs/MintSCAN.guide](docs/MintSCAN.guide).

## Current scope

MintSCAN performs one-page flatbed or feeder scans. The selectors contain
fixed choices:

- Source: Flatbed or Feeder.
- Colour: Colour, Grayscale, or Black & White.
- DPI: 100, 150, 200, 300, 400, or 600.
- Format: JPEG, PNG, or PDF.
- Size: A4, Letter, Legal, or A3.

DPI and colour are checked against the selected source when the scanner
advertises discrete values. Source, format, and size are not yet dynamically
filtered. Multi-page ADF, duplex, HTTPS, IPv6/hostnames, and scan cancellation
are not implemented.

## Building and testing

A Bebbo-style `m68k-amigaos-gcc` toolchain is expected on `PATH`. Override
`CROSS` if the prefix differs.

```sh
make           # build MintScan
make check     # portable HTTP and DNS-SD parser tests
make release   # validate art and stage the Aminet archive under release/
make clean
```

`make release` deliberately refuses placeholder or wrongly typed Workbench
icons. Before release, provide the three DiskObjects described in
[art/README.md](art/README.md). The app icon must request a 131072-byte stack.

## Diagnostics and support

`windows_escl_probe.py` sends the equivalent eSCL requests from a PC on the
same network. It is useful for separating scanner firmware behaviour from an
Amiga networking or filesystem problem.

```sh
python windows_escl_probe.py SCANNER-IP --dpi 400 --color Grayscale8
```

Please [open an issue](https://github.com/boingball/MintSCAN/issues) with the
scanner make/model, AmigaOS version, TCP/IP stack, chosen options, and probe
output where possible.

Implementation notes are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

[MIT](LICENSE) - Copyright (c) 2026 Darren Banfi (boingball).

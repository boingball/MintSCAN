# Changelog

## Unreleased

- Show discovered scanner names without appending their IP address, while
  retaining the endpoint internally for network requests.
- Tighten the status/output panel to match MintPRINT's compact layout, with
  no unused gap above or below the box.

## 1.1.0 - 2026-09-02

- Added Unit0-Unit7 saved scanner profiles.
- Added timeout-bounded connect, send and receive operations with one Wi-Fi
  wake-up retry.
- Added live eSCL ScannerStatus display and richer ScanJobs failures.
- Added an ASL Browse requester and refined the GadTools layout.
- Added 400 DPI to the fixed selector.
- Discovery now retains DNS-SD SRV ports and TXT `rs=` resource roots.
- HTTPS-only `_uscans._tcp` results are no longer offered until AmiSSL is
  supported.
- HTTP text responses now handle case-insensitive headers, interim responses,
  Content-Length and chunked transfer framing.
- Scan downloads now validate disk writes and transfer completion; partial
  files are removed after write, timeout or truncation failures.
- Scoped the Brother MFC-J6930DW `BlackAndWhite1` workaround to that model
  instead of changing B&W to grayscale for every scanner.
- Clear stale model/status/endpoint state when a new discovery or query fails.
- Reduced the requested GUI stack from 384 KiB to 128 KiB.
- Added an AmigaGuide, simplified Installer script, host-side parser tests,
  CI and guarded Aminet release staging.

## 1.0.0

- Initial eSCL/AirScan scan-to-file implementation for classic AmigaOS.

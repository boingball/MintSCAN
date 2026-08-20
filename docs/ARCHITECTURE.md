# MintSCAN architecture

## Why no driver

MintPRINT needs a `DEVS:Printers/` driver because AmigaOS printing has a
device abstraction (`printer.device`) that applications already talk to -
the driver is what translates that into IPP. Scanning has no equivalent
OS-level device class on classic AmigaOS for a network scanner to plug
into, so there is nothing to intercept. MintSCAN is just an application
that speaks eSCL directly and writes the result to a file.

## eSCL flow

```
GET  /eSCL/ScannerCapabilities   -> capabilities XML (formats, resolutions, MakeAndModel)
GET  /eSCL/ScannerStatus         -> idle/processing state (not yet used)
POST /eSCL/ScanJobs              -> ScanSettings XML in, "Location" response header
                                     out (the new job's URL)
GET  {Location}/NextDocument     -> the scanned page, as the requested
                                     DocumentFormat (JPEG/PNG/PDF)
```

`ScanJobs` is a one-shot job today (single page, flatbed). ADF/multi-page
would mean repeating `NextDocument` until the scanner returns 404 (no
more pages) - not implemented yet.

## Discovery

mDNS-SD PTR query for `_uscan._tcp.local` (eSCL over HTTP) and
`_uscans._tcp.local` (eSCL over HTTPS), sent unicast-response (QU bit
set) so this never needs to join the 224.0.0.251 multicast group to see
replies - same shape as MintPRINT's mDNS discovery. Like MintPRINT, this
deliberately does not decode SRV/TXT records; a reply's source address is
enough to populate the picker, and `ScannerCapabilities` after selection
supplies the real details.

eSCL scanners are not reliably discoverable via SSDP the way AirPrint
printers are, so - unlike MintPRINT - there is no SSDP pass here.

mDNS multicast doesn't reach every environment - notably WinUAE's SLIRP
networking, which doesn't route 224.0.0.251 (`no route to 224.0.0.251?`
in the status box). The IP field next to Discover exists for exactly
this: type an IP (or `ip:port`) and click Query to skip discovery
entirely and go straight to `ScannerCapabilities`.

## Known limitations / next steps

- **`NextDocument` download assumes `Content-Length` or a server that
  closes the connection when done.** No `Transfer-Encoding: chunked`
  support yet. Some scanners chunk this response; those will currently
  fail or truncate.
- **HTTPS (`_uscans._tcp`) scanners are discovered but not actually
  reachable** - eSCL-over-TLS needs AmiSSL wired into the HTTP client,
  which isn't done yet. Plain-HTTP eSCL (`_uscan._tcp`, the common case)
  works today.
- **ADF and duplex are not implemented.** `ScanRegions` is always a
  single full-page region at (0,0); there's no multi-page loop.
- **`ScannerCapabilities` is only lightly scraped** (just
  `pwg:MakeAndModel`, for display) rather than fully parsed - the GUI's
  option lists (resolutions, formats, page sizes) are a fixed set picked
  to match what most eSCL scanners advertise, not what a specific
  scanner actually reports it supports.
- **No saved multi-scanner profiles yet** (MintPRINT's Unit0-7 switcher)
  - just a single `ENV:MintSCAN/Unit0`.

## Why every recv() goes through recv_timeout()

The app is single-threaded, so a `recv()` that never returns freezes the
whole GUI - confirmed by testing: after a successful scan, the best-effort
`ScanJobs` `DELETE` cleanup would sit forever in a `while (recv(...) > 0)`
drain loop against a scanner that never replied to `DELETE` and never
closed the connection. `SO_RCVTIMEO` is set on every socket, but isn't
trusted alone to bound `recv()` - same caution the mDNS code already
applies to UDP sockets, just not every bsdsocket.library stack honours it
reliably for TCP either. `recv_timeout()` wraps every `recv()` in this
file with an explicit `WaitSelect`, and `http_delete()` no longer reads a
response at all (fire-and-forget - the scanner will expire the job on its
own if the `DELETE` doesn't land).

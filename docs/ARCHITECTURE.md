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
GET  /eSCL/ScannerStatus         -> idle/processing state (query_scanner_status())
POST /eSCL/ScanJobs              -> ScanSettings XML in, "Location" response header
                                     out (the new job's URL)
GET  {Location}/NextDocument     -> the scanned page, as the requested
                                     DocumentFormat (JPEG/PNG/PDF)
```

`ScanJobs` is a one-shot job today (single page, flatbed). ADF/multi-page
would mean repeating `NextDocument` until the scanner returns 404 (no
more pages) - not implemented yet.

`pwg:ScanRegion`'s `pwg:ContentRegionUnits` was missing for a long time
(only `Height`/`Width`/`XOffset`/`YOffset` were sent) - confirmed by
testing that a real scanner would accept the job and return a valid
image while silently ignoring every other requested field (resolution,
colour mode) and falling back to its own defaults for all of them, no
matter what was actually requested. That's consistent with a
strict/fragile firmware parser failing region validation without an
explicit units element and discarding the rest of the document rather
than rejecting just that one field. `ContentRegionUnits` is now sent
(first child of `ScanRegion`, matching its schema sequence), along with
`scan:Intent` since it's commonly present in working real-world
requests too. If a scanner still ignores requested values after this,
`build_scan_settings_xml()`'s "Requesting: ..." status line is the
place to check the built request actually matches what's expected
next, rather than guessing at the XML shape again.

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

- **`NextDocument` handles `Content-Length`, `Transfer-Encoding: chunked`,
  and a server that just closes the connection when done.** The chunked
  decoder is a plain, tolerant state machine (`chunk_decoder_feed()`) -
  it doesn't parse trailer headers after the terminating 0-length chunk
  (rare in practice; most servers send `0\r\n\r\n` and stop).
- **HTTPS (`_uscans._tcp`) scanners are discovered but not actually
  reachable** - eSCL-over-TLS needs AmiSSL wired into the HTTP client,
  which isn't done yet. Plain-HTTP eSCL (`_uscan._tcp`, the common case)
  works today.
- **ADF and duplex are not implemented.** `ScanRegions` is always a
  single full-page region at (0,0); there's no multi-page loop.
- **`ScannerCapabilities` is only lightly scraped**, not fully parsed.
  `pwg:MakeAndModel` (for display) comes straight from the raw response.
  DPI (every `XResolution>NNN<` value) and Colour mode (whichever of
  RGB24/Grayscale8/BlackAndWhite1 turn up) are scraped from whichever
  Source is currently selected: `extract_source_block()` pulls out the
  substring between `<scan:Platen>`/`<scan:Adf>` and its closing tag
  (matching the Source dropdown) before `scrape_dpi_values()`/
  `scrape_color_values()` run - a scanner can support different values
  per source (e.g. higher DPI or colour modes on the ADF but not the
  flatbed), and scraping the whole document conflated the two, which is
  exactly why an offered/scraped value could still get silently rejected
  by the scanner. Falls back to scraping the whole document if the
  current Source isn't broken out as its own element (some
  capabilities responses don't split them). Source/Format/Size have no
  capability check at all.

  **The DPI and Colour dropdowns themselves are always the same fixed
  list** (`dpi_gui_values`/`color_all_labels`) - they are never rebuilt
  from what's scraped. An earlier version tried swapping a live CYCLE_KIND
  gadget's `GTCY_Labels`/`GTCY_Active` after a capabilities query to show
  only supported values, and testing confirmed this breaks selection
  entirely: the new labels display, but the gadget's internal
  active-index tracking desyncs, so what you pick visually stops
  corresponding to the index the code reads back - values shown but not
  honoured. This is a known GadTools gotcha MintPRINT itself works
  around by never live-updating a cycle gadget's label list once
  created. Instead, `resolve_dpi()`/`resolve_color_value()` (called from
  `build_scan_settings_xml()`) validate the selected value against what
  that Source actually scraped and substitute the closest/first
  supported one if needed. `build_scan_settings_xml()` also always
  prints exactly what it's about to request (DPI/ColorMode/Source), not
  just on substitution - if a scanner still ignores an honestly-supported
  value, that line is the next thing to check, not which value got
  picked client-side.
- **Page sizes (A4/Letter/Legal/A3) are a fixed guess, not derived from
  `MaxWidth`/`MaxHeight`.** A3 was added because a real scanner turned
  out to support it, not because it's queried - a flatbed too small for
  A3 would just get a `ScanRegions` request bigger than its bed.
- **Black & White (`BlackAndWhite1`) is forced to Grayscale for every
  format - confirmed scanner-side, not an Amiga or MintSCAN problem.**
  Two wrong guesses along the way: first a JPEG container limit (JPEG
  can't encode 1-bit data - disproven when PNG, which can, showed the
  identical corruption), then an Amiga picture-datatype limitation
  (based on `windows_escl_probe.py <ip> --color BlackAndWhite1`'s saved
  JPEG opening without error under Python's PIL - which turned out to
  prove only that the *container* was well-formed, not that the *image*
  was correct). Actually looking at that saved file settled it for real:
  full RGB colour - skin tones, a red apple, visible horizontal banding
  - despite the request explicitly asking for `BlackAndWhite1`, fetched
  by a plain PC script with no Amiga anywhere in the path. The scanner
  simply isn't honouring `BlackAndWhite1` as a `ColorMode` value, full
  stop - nothing to fix in MintSCAN's request or its (nonexistent, it
  never decodes the image) processing of the response.
  `build_scan_settings_xml()` substitutes Grayscale8 whenever
  BlackAndWhite1 is selected and logs it, the same way an unsupported
  DPI/Colour value gets substituted and logged elsewhere - the same
  scanner does honour Grayscale8 correctly. Worth revisiting only
  against a firmware update, or a different scanner, that's confirmed to
  actually produce a clean `BlackAndWhite1` image.

## Saved profiles: Unit0-7

Same idea as MintPRINT's Unit0-7 printer switcher: up to 8 saved scanner
profiles at `ENV(ARC):MintSCAN/UnitN`, picked with the `Unit:` cycle
gadget. Unlike MintPRINT there's no background driver with its own idea
of which Unit is "live" - MintSCAN is the only reader of these files, so
whichever Unit is currently loaded in the GUI is simply what `Scan` uses
next; there's no separate Activate step. Switching the dropdown
(`reload_current_unit()`) resets every field to MintScan's built-in
defaults, then loads that Unit's saved file over them if one exists -
so an empty slot doesn't inherit whatever the previous Unit had typed
into it. The dropdown's own labels (`refresh_unit_dropdown()`) show each
slot's saved model name, peeked straight off disk without disturbing the
live GUI state, matching MintPRINT's `peek_unit_model()`/
`refresh_unit_dropdown()` pair.

## Unit vs. Scanner: two dropdowns, two different jobs

These look redundant at a glance (both can end up showing the same
scanner) but answer different questions. **Unit** is "which of my eight
*saved* profiles am I looking at" - a config file on disk, picked with
`reload_current_unit()`, independent of anything on the network right
now. **Scanner** is "which of the scanners *just discovered* on the LAN
(or manually IP'd/Queried) am I about to use" - an in-memory list from
the last Discover click, picked with `select_discovered_scanner()`. The
normal flow is Discover -> pick from Scanner -> Save Config, which
writes the picked scanner into whichever Unit slot is current - collapsing
them into one control would lose either "switch to a profile I saved
weeks ago without touching the network" or "here's what's on the LAN
right now, none of which I've saved yet". `load_unit_config()` seeds
Scanner's list with a single "saved" entry so the two agree right after
a Unit switch, before any live Discover has run.

## Save-to file requester

`do_browse_savepath()` opens a standard ASL file requester
(`asl.library`, `AllocAslRequestTags(ASL_FileRequest, ...)`) instead of
requiring `savepath_buffer` to be typed by hand. `FilePart()` splits the
current path into a starting drawer/file for the requester's
`ASLFR_InitialDrawer`/`ASLFR_InitialFile`, and a result is rebuilt with
`AddPart()` rather than plain string concatenation, since `AddPart()`
already knows whether the drawer ends in `/`/`:` and only inserts a
separator when it doesn't. `asl.library` is opened once at startup but
treated as optional - if it's missing, Browse just reports that instead
of the app refusing to start over what's a convenience on top of typing
a path, not a requirement.

## Wi-Fi scanners that sleep between jobs

`http_connect()` bounds every connect attempt with `connect_with_timeout()`
- a non-blocking `connect()` polled via `WaitSelect()` rather than trusting
however long the stack's own blocking connect() feels like taking - and
retries once on failure before giving up. This is the same fix MintPRINT
shipped for HP OfficeJet/Envy-class Wi-Fi printers that drop their radio
into power-save between jobs: mDNS discovery still gets a reply because
the radio wakes for multicast traffic, but the first real TCP SYN
afterwards can be slow enough to blow past a single connect attempt even
though the same device answers almost immediately once its radio is
awake. A genuinely dead endpoint still fails fast via
ECONNREFUSED/host-unreachable well inside the timeout, so the retry
costs little.

## Why every recv() and send() goes through a *_timeout() wrapper

The app is single-threaded, so a `recv()` or `send()` that never returns
freezes the whole GUI - confirmed by testing: after a successful scan, the
best-effort `ScanJobs` `DELETE` cleanup would sit forever in a
`while (recv(...) > 0)` drain loop against a scanner that never replied to
`DELETE` and never closed the connection. `SO_RCVTIMEO` is set on every
socket, but isn't trusted alone to bound `recv()` - same caution the mDNS
code already applies to UDP sockets, just not every bsdsocket.library
stack honours it reliably for TCP either. `recv_timeout()` wraps every
`recv()` in this file with an explicit `WaitSelect`, and `http_delete()`
no longer reads a response at all (fire-and-forget - the scanner will
expire the job on its own if the `DELETE` doesn't land).

`send()` had the same gap for a while (MintPRINT's own `ipp_client.c`
shipped with exactly this: `connect()`/`recv()` timeout-bounded, `send()`
still a plain blocking call, fixed in its 1.2.5). A scanner that accepts a
connection and then stops draining its TCP receive window mid-request -
not just mid-response - blocks `send()` the same way a stalled `recv()`
blocks the GUI. `send_timeout()` mirrors `recv_timeout()`
(`WaitSelect`-bounded, looped for short writes) and is used everywhere
`send()` used to be called directly.

## Scanner status (ScannerStatus)

`query_scanner_status()` reads `/eSCL/ScannerStatus` and records the
top-level `pwg:State` (Idle/Processing/Testing/Stopped/Down) - not the
per-job state further down the same document, which needs a job UUID to
correlate and isn't tracked here. It runs after every successful
`ScannerCapabilities` query and again if `ScanJobs` fails, so a bare HTTP
status code ("ScanJobs failed (status 503)") gets a real reason
alongside it when the scanner offers one ("... - scanner reports:
Processing"). Shown live in the **Status:** field next to Model - the
same idea as MintPRINT surfacing `printer-state` next to its ink/toner
strip. Not fatal if the request fails or the element is missing;
`ScannerStatus` support is optional in some firmware.

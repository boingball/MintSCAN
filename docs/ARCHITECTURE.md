# MintSCAN architecture

## Application shape

Classic AmigaOS has a standard printing device interface but no equivalent
scanner device class. MintSCAN is therefore a standalone GadTools application,
not a driver. It speaks eSCL directly through `bsdsocket.library` and streams
the scanner-produced document to an AmigaDOS file.

## eSCL transaction

| Step | Request | Purpose |
|---|---|---|
| 1 | `GET {root}/ScannerCapabilities` | Read model and advertised options |
| 2 | `GET {root}/ScannerStatus` | Show the top-level scanner state |
| 3 | `POST {root}/ScanJobs` | Create a job with compact ScanSettings XML |
| 4 | `GET {Location}/NextDocument` | Stream one JPEG, PNG, or PDF page |
| 5 | `DELETE {Location}` | Best-effort job cleanup |

The conventional root is `/eSCL`, but discovery retains the DNS-SD TXT
`rs=` value and SRV port. A relative ScanJobs `Location` is resolved below
that root; an absolute path or URL is normalised to its request path.

All connect, send and receive operations are bounded with `WaitSelect()`.
Connections receive one retry because sleeping Wi-Fi scanners may wake for the
mDNS query before accepting TCP.

## Discovery

MintSCAN sends a DNS-SD PTR query for `_uscan._tcp.local` with the QU bit.
`src/mdns_endpoint.c` safely decodes compressed PTR, SRV and TXT records and
extracts:

- the service instance;
- SRV port;
- TXT `rs=` eSCL root;
- TXT `ty=` display label.

The IPv4 source address of the mDNS reply is used for the connection.
Endpoints are de-duplicated by address, port and root. TLS-only
`_uscans._tcp` services are intentionally not listed until AmiSSL is wired
into the HTTP client.

Manual Query accepts an IPv4 address and optional port and uses `/eSCL`.

## HTTP and file integrity

`src/http_response.c` provides allocation-free, case-insensitive HTTP/1.x
helpers. Text requests handle interim 1xx replies, Content-Length, chunked
transfer encoding and mixed-case headers.

`NextDocument` uses a streaming chunk decoder so an image is never buffered
whole in memory. Success requires one of:

- exactly the declared Content-Length;
- a terminal zero chunk with valid chunk framing; or
- an orderly connection close when no framing header was supplied.

Every AmigaDOS `Write()` and final `Close()` is checked. A timeout,
malformed response, truncated body, disk-full condition or close error removes
the partial output file.

## Capabilities and request choices

The live GadTools cycle labels remain fixed. Replacing cycle label arrays after
creation can desynchronise the displayed selection from the active index on the
target GadTools versions.

At scan time MintSCAN lightly scrapes the selected `scan:Platen` or
`scan:Adf` block:

- a supported discrete DPI is used, or the closest advertised DPI is chosen;
- a supported colour mode is used, or the first advertised mode is chosen;
- `DocumentFormatExt` is emitted only when advertised for that source.

Source, document format and region size are currently fixed lists. The XML
uses `escl:ThreeHundredthsOfInches` regions and is deliberately compact for
firmware known to accept a job while silently defaulting malformed settings.

Brother MFC-J6930DW firmware has been observed to advertise
`BlackAndWhite1` but return banded RGB data. Only that model is substituted
to `Grayscale8`; other scanners receive the selected 1-bit mode.

## Saved state

Unit0 through Unit7 are independent profiles in
`ENV:MintSCAN/UnitN` and `ENVARC:MintSCAN/UnitN`. Each stores the endpoint
address, port and root, model, selected options and destination path. Switching
Unit resets defaults before loading, preventing fields from leaking between
profiles.

The Scanner cycle is separate: it represents the most recent live discovery
(or one manual endpoint). A new discovery clears the previously active
endpoint, model, status and capabilities so failure cannot leave an old scanner
scan-ready.

## Portable tests

`make check` builds and runs host tests for the protocol-only modules:

- HTTP status/header/framing and in-place chunk decoding;
- DNS name compression plus PTR/SRV/TXT endpoint extraction.

The Amiga GUI and bsdsocket integration still require the m68k cross-toolchain
and target/emulator testing.

## Current limitations

- Plain HTTP only; no AmiSSL/HTTPS.
- One page per operation; no ADF continuation loop or duplex.
- No scan cancellation.
- IPv4 literals only for manual Query.
- Lightweight XML scraping rather than a full XML parser.
- Fixed source, format and page-size lists.

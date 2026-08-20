#!/usr/bin/env python3
"""windows_escl_probe.py - eSCL diagnostic probe, run from a Windows PC
(or any machine with Python 3) on the same network as the scanner.

Isolates scanner-side vs MintSCAN-side issues without needing Amiga
tooling in the loop: fetches ScannerCapabilities, POSTs a ScanSettings
request built the same way MintSCAN's build_scan_settings_xml() does
(same ContentRegionUnits/Intent/element order), and downloads
NextDocument - printing raw status codes, headers, and the actual
image's resolution/mode at every step, so a mismatch is visible
immediately instead of requiring a full Amiga round-trip to check.

Usage:
    python windows_escl_probe.py <scanner-ip> [options]

Examples:
    python windows_escl_probe.py 192.168.0.71
    python windows_escl_probe.py 192.168.0.71 --dpi 300 --color RGB24
    python windows_escl_probe.py 192.168.0.71 --dpi 400 --color BlackAndWhite1 --source Platen

No third-party dependencies - only needs a standard Python 3 install.
If Pillow (PIL) is installed, the downloaded image's actual resolution
and mode get printed too; otherwise just its size in bytes.
"""

import argparse
import http.client
import re
import sys
from urllib.parse import urlparse

# (width, height) in eSCL's fixed 300-units/inch ScanRegion convention -
# same values as size_width_300[]/size_height_300[] in MintScan.c.
SIZE_300_UNITS = {
    "A4": (2480, 3507),
    "Letter": (2550, 3300),
    "Legal": (2550, 4200),
    "A3": (3508, 4961),
}

FORMAT_EXT = {
    "image/jpeg": "jpg",
    "image/png": "png",
    "application/pdf": "pdf",
}


def connect(host, port):
    return http.client.HTTPConnection(host, port, timeout=15)


def request(conn, method, path, body=None, headers=None):
    headers = dict(headers or {})
    headers.setdefault("Host", conn.host)
    headers.setdefault("Connection", "close")
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    data = resp.read()
    return resp, data


def dump_headers(resp):
    for k, v in resp.getheaders():
        print(f"    {k}: {v}")


def scrape_resolutions(xml_text, tag):
    """Mirrors scrape_dpi_values() in MintScan.c: XResolution values
    found within the given source element (scan:Platen / scan:Adf),
    falling back to the whole document if that element isn't present."""
    m = re.search(rf"<{re.escape(tag)}[^>]*>(.*?)</{re.escape(tag)}>", xml_text, re.S)
    scope = m.group(1) if m else xml_text
    values = sorted(set(int(v) for v in re.findall(r"XResolution>(\d+)<", scope)))
    return values, (m is not None)


def scrape_colors(xml_text, tag):
    m = re.search(rf"<{re.escape(tag)}[^>]*>(.*?)</{re.escape(tag)}>", xml_text, re.S)
    scope = m.group(1) if m else xml_text
    return [c for c in ("RGB24", "Grayscale8", "BlackAndWhite1") if c in scope], (m is not None)


def build_scan_settings_xml(source, color, dpi, fmt, size):
    width, height = SIZE_300_UNITS[size]
    source_tag = "Platen" if source == "Platen" else "Feeder"
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<scan:ScanSettings xmlns:scan="http://schemas.hp.com/imaging/escl/2011/05/03" '
        'xmlns:pwg="http://www.pwg.org/schemas/2010/12/sm">\n'
        "<pwg:Version>2.0</pwg:Version>\n"
        "<scan:Intent>Document</scan:Intent>\n"
        "<pwg:ScanRegions>\n"
        "<pwg:ScanRegion>\n"
        "<pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>\n"
        f"<pwg:Height>{height}</pwg:Height>\n"
        f"<pwg:Width>{width}</pwg:Width>\n"
        "<pwg:XOffset>0</pwg:XOffset>\n"
        "<pwg:YOffset>0</pwg:YOffset>\n"
        "</pwg:ScanRegion>\n"
        "</pwg:ScanRegions>\n"
        f"<pwg:InputSource>{source_tag}</pwg:InputSource>\n"
        f"<scan:ColorMode>{color}</scan:ColorMode>\n"
        f"<scan:XResolution>{dpi}</scan:XResolution>\n"
        f"<scan:YResolution>{dpi}</scan:YResolution>\n"
        f"<pwg:DocumentFormat>{fmt}</pwg:DocumentFormat>\n"
        "</scan:ScanSettings>\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="scanner IP or hostname")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--source", choices=["Platen", "Feeder"], default="Platen")
    ap.add_argument("--color", choices=["RGB24", "Grayscale8", "BlackAndWhite1"], default="BlackAndWhite1")
    ap.add_argument("--dpi", type=int, default=400)
    ap.add_argument("--format", dest="fmt", choices=list(FORMAT_EXT), default="image/jpeg")
    ap.add_argument("--size", choices=list(SIZE_300_UNITS), default="A4")
    ap.add_argument("--out", default=None, help="output filename (default: scan_probe_output.<ext>)")
    ap.add_argument("--skip-scan", action="store_true", help="only fetch capabilities/status, don't scan")
    args = ap.parse_args()

    out_path = args.out or f"scan_probe_output.{FORMAT_EXT[args.fmt]}"

    print(f"=== GET /eSCL/ScannerCapabilities ({args.host}:{args.port}) ===")
    conn = connect(args.host, args.port)
    resp, data = request(conn, "GET", "/eSCL/ScannerCapabilities")
    print(f"Status: {resp.status} {resp.reason}")
    dump_headers(resp)
    caps_text = data.decode("utf-8", errors="replace")
    with open("escl_capabilities.xml", "w", encoding="utf-8") as f:
        f.write(caps_text)
    print(f"Saved raw response -> escl_capabilities.xml ({len(data)} bytes)")

    m = re.search(r"MakeAndModel>(.*?)<", caps_text)
    print(f"MakeAndModel: {m.group(1) if m else '(not found)'}")

    for tag in ("scan:Platen", "scan:Adf"):
        res, found = scrape_resolutions(caps_text, tag)
        cols, _ = scrape_colors(caps_text, tag)
        label = tag.split(":")[1]
        print(f"{label}: element {'found' if found else 'NOT found (scraped whole doc)'}")
        print(f"  XResolution values seen: {res if res else '(none found)'}")
        print(f"  ColorMode values seen:   {cols if cols else '(none found)'}")
    conn.close()

    print()
    print("=== GET /eSCL/ScannerStatus ===")
    conn = connect(args.host, args.port)
    try:
        resp, data = request(conn, "GET", "/eSCL/ScannerStatus")
        print(f"Status: {resp.status} {resp.reason}")
        dump_headers(resp)
        print(data.decode("utf-8", errors="replace"))
    except Exception as e:
        print(f"(failed: {e})")
    conn.close()

    if args.skip_scan:
        return

    print()
    print(f"=== POST /eSCL/ScanJobs (source={args.source} color={args.color} dpi={args.dpi} "
          f"format={args.fmt} size={args.size}) ===")
    xml = build_scan_settings_xml(args.source, args.color, args.dpi, args.fmt, args.size)
    print("Request body:")
    print(xml)

    conn = connect(args.host, args.port)
    resp, data = request(
        conn, "POST", "/eSCL/ScanJobs", body=xml.encode("utf-8"),
        headers={"Content-Type": "text/xml", "Content-Length": str(len(xml))},
    )
    print(f"Status: {resp.status} {resp.reason}")
    dump_headers(resp)
    location = resp.getheader("Location")
    conn.close()

    if resp.status != 201 or not location:
        print("ScanJobs did not return 201 Created with a Location header - stopping.")
        if data:
            print("Response body:", data.decode("utf-8", errors="replace"))
        return

    parsed = urlparse(location)
    job_path = parsed.path if parsed.scheme else location
    print(f"Job path: {job_path}")

    print()
    print(f"=== GET {job_path}/NextDocument ===")
    conn = connect(args.host, args.port)
    resp, data = request(conn, "GET", job_path + "/NextDocument")
    print(f"Status: {resp.status} {resp.reason}")
    dump_headers(resp)
    conn.close()

    if resp.status == 200 and data:
        with open(out_path, "wb") as f:
            f.write(data)
        print(f"Saved {len(data)} bytes -> {out_path}")
        try:
            from PIL import Image
            img = Image.open(out_path)
            dpi_info = img.info.get("dpi", "(no DPI tag)")
            print(f"Actual image: {img.size[0]}x{img.size[1]} px, mode={img.mode}, dpi={dpi_info}")
        except ImportError:
            print("(install Pillow - 'pip install pillow' - to also print actual resolution/mode here)")
        except Exception as e:
            print(f"(couldn't inspect image: {e})")
    else:
        print("No image data returned.")

    print()
    print(f"=== DELETE {job_path} (best-effort cleanup) ===")
    conn = connect(args.host, args.port)
    try:
        resp, _ = request(conn, "DELETE", job_path)
        print(f"Status: {resp.status} {resp.reason}")
    except Exception as e:
        print(f"(failed, ignored: {e})")
    conn.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(1)

#!/usr/bin/env python3
"""One-time source integration for issue #8, run on the feature branch."""
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected one occurrence in {path} (found {count}): {old[:90]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


src = Path("src/MintScan.c")
mk = Path("Makefile")
replace_once(src,
    '#include "mdns_endpoint.h"\n',
    '#include "mdns_endpoint.h"\n#include "escl_format.h"\n')
old_ext = '''static BOOL source_uses_document_format_ext(void) {
    static char scoped[8192];
    const char *scope;

    if (!have_capabilities) return FALSE;

    scoped[0] = '\\0';
    extract_source_block(capabilities_xml, source_capability_tags[source_index],
                         scoped, sizeof(scoped));
    scope = scoped[0] ? scoped : capabilities_xml;

    return strstr(scope, "DocumentFormatExt") != NULL;
}
'''
new_ext = '''/* Only send the extension if this source advertises this exact MIME.
   Advertising JPEG in DocumentFormatExt is not support for PNG there. */
static BOOL source_uses_document_format_ext(const char *mime) {
    static char scoped[MAX_BUFFER];
    const char *scope;

    if (!have_capabilities) return FALSE;

    scoped[0] = '\\0';
    extract_source_block(capabilities_xml, source_capability_tags[source_index],
                         scoped, sizeof(scoped));
    scope = scoped[0] ? scoped : capabilities_xml;

    return ms_escl_has_format_value(scope, "scan:DocumentFormatExt", mime) != 0;
}

/* Prevent predictable HTTP 409s for PNG when this source advertises only
   JPEG/PDF. Never silently create a JPEG and give it a .png extension.
   An old scanner with no format list is left to handle the request. */
static BOOL source_supports_selected_png(void) {
    static char scoped[MAX_BUFFER];
    const char *scope;
    int known = 0;

    if (format_index != 1 || !have_capabilities) return TRUE;
    scoped[0] = '\\0';
    extract_source_block(capabilities_xml, source_capability_tags[source_index],
                         scoped, sizeof(scoped));
    scope = scoped[0] ? scoped : capabilities_xml;
    if (ms_escl_format_supported(scope, "image/png", &known) || !known)
        return TRUE;
    printf("PNG is not advertised for %s - select JPEG or PDF instead\\n",
           (char *)source_labels[source_index]);
    return FALSE;
}
'''
replace_once(src, old_ext, new_ext)
replace_once(src,
    'BOOL use_document_format_ext = source_uses_document_format_ext();',
    'BOOL use_document_format_ext = source_uses_document_format_ext(mime);')
replace_once(src,
    '''    if (!scanner_host[0]) {
        printf("No scanner selected - use Discover first\\n");
        return;
    }

    build_scan_settings_xml(xml, sizeof(xml));''',
    '''    if (!scanner_host[0]) {
        printf("No scanner selected - use Discover first\\n");
        return;
    }
    if (!source_supports_selected_png()) return;

    build_scan_settings_xml(xml, sizeof(xml));''')
replace_once(src,
    '''    if (status != 201 || !location[0]) {
        query_scanner_status(scanner_host, scanner_port);''',
    '''    if (status != 201 || !location[0]) {
        if (status == 409 && format_index == 1)
            printf("Scanner rejected PNG settings - check its advertised formats\\n");
        query_scanner_status(scanner_host, scanner_port);''')
replace_once(mk,
    '.PHONY: all help check test-http test-mdns check-art release clean',
    '.PHONY: all help check test-http test-mdns test-format check-art release clean')
replace_once(mk,
    '  make check     - run host-side HTTP and DNS-SD tests',
    '  make check     - run host-side HTTP, DNS-SD and format tests')
replace_once(mk,
    'MintScan: src/MintScan.c src/http_response.c src/http_response.h src/mdns_endpoint.c src/mdns_endpoint.h',
    'MintScan: src/MintScan.c src/escl_format.h src/http_response.c src/http_response.h src/mdns_endpoint.c src/mdns_endpoint.h')
replace_once(mk,
    'check: test-http test-mdns',
    '''test-format: | $(TEST_DIR)
\t$(HOST_CC) $(HOST_CFLAGS) -Isrc -o $(TEST_DIR)/test_escl_format tests/test_escl_format.c
\t$(TEST_DIR)/test_escl_format

check: test-http test-mdns test-format''')
print("Issue #8 source and Makefile changes applied")

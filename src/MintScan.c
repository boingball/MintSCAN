/* MintSCAN - eSCL (AirScan/AirPrint Scan) scanning for classic AmigaOS.
   Discovers eSCL scanners on the LAN (mDNS), queries ScannerCapabilities,
   and scans a single page straight to a file via ScanJobs/NextDocument.

   Sibling project to MintPRINT - same bsdsocket/GadTools idioms, but a
   plain application (no printer.device-style driver: there is no
   AmigaOS device abstraction for scanners to hook into). See
   docs/ARCHITECTURE.md for the eSCL flow and known limitations. */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
typedef long ssize_t;
#include <clib/alib_protos.h>
#include <proto/bsdsocket.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define USED __attribute__((used))

/* Classic AmigaOS / libnix stack request - same convention as MintPRINT.
   The "$STACK:" cookie is honoured by newer startup code; classic m68k
   GCC/libnix runtimes read the __stack variable instead. Keep this
   comfortably large: GadTools + bsdsocket + the eSCL/chunk-decoder call
   chains go several frames deep. 384 KiB = 393216 bytes. */
unsigned long __stack = 393216UL;
static const char USED min_stack[] = "$STACK:393216";

/* --------------------------------------------------------------------
 * Constants, gadget IDs, globals
 * ----------------------------------------------------------------- */

#define GAD_SCANNER_DROPDOWN 1
#define GAD_DISCOVER_BUTTON  2
#define GAD_MODEL_DISPLAY    3
#define GAD_SOURCE_DROPDOWN  4
#define GAD_COLOR_DROPDOWN   5
#define GAD_DPI_DROPDOWN     6
#define GAD_FORMAT_DROPDOWN  7
#define GAD_SIZE_DROPDOWN    8
#define GAD_SAVEPATH_STRING  9
#define GAD_SCAN_BUTTON      10
#define GAD_SAVE_BUTTON      11
#define GAD_EXIT_BUTTON      12
#define GAD_IP_STRING        13
#define GAD_QUERY_BUTTON     14

#define MAX_DISCOVERY_RESULTS 16
#define MAX_DPI_OPTIONS 10
#define MAX_BUFFER    65536   /* capabilities / ScanJobs response scratch */
#define MAX_OUTPUT_LINES 8
#define MAX_OUTPUT_LINE_LENGTH 55
#define OUTPUT_TOP    245
#define OUTPUT_LEFT   10
#define OUTPUT_LINE_H 10
#define OUTPUT_RIGHT  (window->Width - 20)

struct DiscoveredScanner {
    char ip[16];
    char label[80];
};

static struct DiscoveredScanner discovered[MAX_DISCOVERY_RESULTS];
static int discovered_count = 0;
static STRPTR scanner_label_ptrs[MAX_DISCOVERY_RESULTS + 1];
static char scanner_label_storage[MAX_DISCOVERY_RESULTS][80];
static STRPTR *scanner_dropdown_labels = NULL;

/* The scanner currently selected in the GUI - may come from discovery,
   or from a saved profile loaded at startup before any Discover click. */
static char scanner_host[64] = "";
static int  scanner_port = 80;
static char scanner_make_model[96] = "";
static BOOL have_capabilities = FALSE;

/* Raw ScannerCapabilities body from the last successful query, kept
   around so resolve_dpi()/resolve_color_value() can re-scope their
   scrape to whichever Source is selected *at scan time* - not
   necessarily the one that was active when the query ran. */
static char capabilities_xml[MAX_BUFFER] = "";

/* Option tables. Labels are what GadTools shows; the parallel array is
   what actually goes into the eSCL request. Kept as a fixed set rather
   than parsed from ScannerCapabilities - see docs/ARCHITECTURE.md. */
static STRPTR source_labels[] = { (STRPTR)"Flatbed", (STRPTR)"Feeder (ADF)", NULL };
static const char *source_values[] = { "Platen", "Feeder" };
/* The ScannerCapabilities element each Source scans under - not the
   same string as pwg:InputSource above (that's "Feeder", this is
   "Adf"). Used to scope the DPI/Colour capability scrape. */
static const char *source_capability_tags[] = { "scan:Platen", "scan:Adf" };

/* Colour dropdown - a fixed, never-swapped list (see the long comment on
   dpi_gui_values below for why it stays fixed). color_index indexes both
   arrays directly (1:1, no filtering). */
#define COLOR_MODE_COUNT 3
static STRPTR color_all_labels[COLOR_MODE_COUNT + 1] = {
    (STRPTR)"Colour", (STRPTR)"Grayscale", (STRPTR)"Black & White", NULL
};
static const char *color_all_values[COLOR_MODE_COUNT] = { "RGB24", "Grayscale8", "BlackAndWhite1" };

/* DPI dropdown - a fixed, never-swapped list. Tried making this (and
   Colour, above) reflect ScannerCapabilities by swapping the live
   gadget's GTCY_Labels/GTCY_Active after a query - confirmed by testing
   that this breaks selection entirely (values shown but not honoured).
   That matches a documented GadTools gotcha MintPRINT already works
   around: repeatedly swapping a CYCLE_KIND gadget's label array while
   it's live can desync its internal active-index tracking on this
   NDK/GadTools combo, even though nothing crashes and the new labels
   *display* correctly. Source/Format/Size never get swapped and have
   never shown this problem - Colour and DPI were the only two gadgets
   that did. Instead: keep the dropdown static, and validate/snap
   against what the scanner actually supports right when the request is
   built - see resolve_dpi() and resolve_color_value(). */
static const int dpi_gui_values[] = { 100, 150, 200, 300, 600 };
static STRPTR dpi_gui_labels[] = {
    (STRPTR)"100", (STRPTR)"150", (STRPTR)"200", (STRPTR)"300", (STRPTR)"600", NULL
};
#define DPI_GUI_COUNT 5

static STRPTR format_labels[] = { (STRPTR)"JPEG", (STRPTR)"PNG", (STRPTR)"PDF", NULL };
static const char *format_mimes[] = { "image/jpeg", "image/png", "application/pdf" };
static const char *format_extensions[] = { "jpg", "png", "pdf" };

/* Region sizes in eSCL's units: hundredths of an inch at 300 units/inch
   regardless of scan resolution (per the eSCL spec's ScanRegions). */
static STRPTR size_labels[] = { (STRPTR)"A4", (STRPTR)"Letter", (STRPTR)"Legal", (STRPTR)"A3", NULL };
static const int size_width_300[]  = { 2480, 2550, 2550, 3508 };
static const int size_height_300[] = { 3507, 3300, 4200, 4961 };

static int source_index = 0;
static int color_index = 0;
static int dpi_index = 3;   /* 300 dpi */
static int format_index = 0; /* JPEG */
static int size_index = 0;  /* A4 */

static char savepath_buffer[256] = "RAM:scan001.jpg";

/* Manual entry, for setups where mDNS multicast doesn't reach the Amiga
   (e.g. WinUAE's SLIRP networking, which doesn't route 224.0.0.251).
   GAD_IP_STRING's initial text - kept in sync with what's actually typed
   via sync_string_gadget(), since GTST_String only seeds a string
   gadget's content at creation and doesn't alias this buffer for live
   edits. Accepts "host" or "host:port"; port defaults to 80. */
static char ip_entry_buffer[70] = "";

static char output_buffer[MAX_OUTPUT_LINES][MAX_OUTPUT_LINE_LENGTH];
static int output_line = 0;

static struct Window *window = NULL;
static struct Gadget *glist = NULL;
static struct Screen *screen = NULL;
static void *vi = NULL;
static struct TextFont *font = NULL;
/* Not static: proto/*.h already declares these extern (non-static) - this
   is the actual definition, matching MintPRINT's own convention. */
struct Library *SocketBase = NULL;
struct Library *GadToolsBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
static BOOL operation_in_progress = FALSE;

static struct TextAttr Topaz80 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextAttr Topaz60 = { (STRPTR)"topaz.font", 6, 0, 0 };

/* status/progress output goes to the on-screen box, never a console -
   this may be launched from Workbench, where there is no console. */
static void custom_printf(const char *format, ...);
#define printf custom_printf

/* --------------------------------------------------------------------
 * Status output box
 * ----------------------------------------------------------------- */

static void redraw_output_box(void) {
    struct RastPort *rp;
    int line_height, top, bottom, start_line, i;

    if (!window) return;

    rp = window->RPort;
    if (font) SetFont(rp, font);
    SetAPen(rp, 1);
    SetBPen(rp, 0);
    SetDrMd(rp, JAM2);

    line_height = font->tf_YSize + 2;
    top = OUTPUT_TOP;
    bottom = top + (MAX_OUTPUT_LINES * line_height) - 1;

    SetAPen(rp, 1);
    RectFill(rp, OUTPUT_LEFT - 2, top - 2, OUTPUT_RIGHT + 2, top - 1);
    RectFill(rp, OUTPUT_LEFT - 2, bottom + 1, OUTPUT_RIGHT + 2, bottom + 2);
    RectFill(rp, OUTPUT_LEFT - 2, top - 2, OUTPUT_LEFT - 1, bottom + 2);
    RectFill(rp, OUTPUT_RIGHT + 1, top - 2, OUTPUT_RIGHT + 2, bottom + 2);

    SetAPen(rp, 0);
    RectFill(rp, OUTPUT_LEFT, top, OUTPUT_RIGHT, bottom);

    start_line = (output_line > MAX_OUTPUT_LINES) ? (output_line - MAX_OUTPUT_LINES) : 0;
    for (i = 0; i < MAX_OUTPUT_LINES && (start_line + i) < output_line; i++) {
        int y = top + (i * line_height) + font->tf_Baseline;
        Move(rp, OUTPUT_LEFT, y);
        SetAPen(rp, 1);
        Text(rp, (CONST_STRPTR)output_buffer[start_line + i], strlen(output_buffer[start_line + i]));
    }
}

static void custom_printf(const char *format, ...) {
    va_list args;
    char temp[256];
    size_t len;
    int i;

    if (strcmp(format, "CLEAR") == 0) {
        output_line = 0;
        redraw_output_box();
        return;
    }

    va_start(args, format);
    vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);

    len = strlen(temp);
    if (len > 0 && temp[len - 1] == '\n') {
        temp[len - 1] = '\0';
    }

    if (output_line >= MAX_OUTPUT_LINES) {
        for (i = 0; i < MAX_OUTPUT_LINES - 1; i++) {
            strncpy(output_buffer[i], output_buffer[i + 1], MAX_OUTPUT_LINE_LENGTH);
        }
        output_line = MAX_OUTPUT_LINES - 1;
    }

    strncpy(output_buffer[output_line], temp, MAX_OUTPUT_LINE_LENGTH - 1);
    output_buffer[output_line][MAX_OUTPUT_LINE_LENGTH - 1] = '\0';
    output_line++;

    redraw_output_box();
}

/* Drains pending GUI events without acting on them - called from inside
   blocking network loops so the window stays responsive (repaints,
   moves) while a discovery poll or HTTP request is in flight. */
static void drain_gui_events(void) {
    if (window) {
        struct IntuiMessage *imsg;
        while ((imsg = GT_GetIMsg(window->UserPort))) {
            GT_ReplyIMsg(imsg);
        }
    }
}

/* --------------------------------------------------------------------
 * Saved profile: ENV:MintSCAN/Unit0 (and ENVARC: for persistence)
 * ----------------------------------------------------------------- */

static void trim_config_line(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                 s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static BOOL ensure_config_dir(CONST_STRPTR name) {
    BPTR lock = Lock(name, ACCESS_READ);
    if (lock) { UnLock(lock); return TRUE; }
    lock = CreateDir(name);
    if (!lock) return FALSE;
    UnLock(lock);
    return TRUE;
}

static int find_label_index(const char **values, int count, const char *value) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(values[i], value) == 0) return i;
    }
    return -1;
}

static BOOL write_config_file(CONST_STRPTR filename) {
    BPTR file;
    char line[256];

    file = Open(filename, MODE_NEWFILE);
    if (!file) return FALSE;

    FPuts(file, (STRPTR)"# MintSCAN Unit0 - written by MintScan\n");
    snprintf(line, sizeof(line), "HOST=%s\n", scanner_host);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "PORT=%d\n", scanner_port);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "MODEL=%s\n", scanner_make_model);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "SOURCE=%s\n", source_values[source_index]);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "COLORMODE=%s\n", color_all_values[color_index]);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "RESOLUTION=%d\n", dpi_gui_values[dpi_index]);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "FORMAT=%s\n", format_mimes[format_index]);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "PAGESIZE=%s\n", (char *)size_labels[size_index]);
    FPuts(file, (STRPTR)line);
    snprintf(line, sizeof(line), "SAVEPATH=%s\n", savepath_buffer);
    FPuts(file, (STRPTR)line);

    Close(file);
    return TRUE;
}

static BOOL save_config(void) {
    BOOL env_ok, envarc_ok;

    if (!ensure_config_dir((CONST_STRPTR)"ENV:MintSCAN")) {
        printf("Could not create ENV:MintSCAN\n");
        return FALSE;
    }
    if (!ensure_config_dir((CONST_STRPTR)"ENVARC:MintSCAN")) {
        printf("Could not create ENVARC:MintSCAN\n");
        return FALSE;
    }

    env_ok = write_config_file((CONST_STRPTR)"ENV:MintSCAN/Unit0");
    envarc_ok = write_config_file((CONST_STRPTR)"ENVARC:MintSCAN/Unit0");
    return env_ok && envarc_ok;
}

static void load_config(void) {
    BPTR file;
    char line[256];
    int idx;

    file = Open((CONST_STRPTR)"ENV:MintSCAN/Unit0", MODE_OLDFILE);
    if (!file) file = Open((CONST_STRPTR)"ENVARC:MintSCAN/Unit0", MODE_OLDFILE);
    if (!file) return;

    while (FGets(file, (STRPTR)line, sizeof(line))) {
        char *value;
        trim_config_line(line);
        if (!line[0] || line[0] == '#') continue;

        if (strncmp(line, "HOST=", 5) == 0) {
            value = line + 5;
            strncpy(scanner_host, value, sizeof(scanner_host) - 1);
            scanner_host[sizeof(scanner_host) - 1] = '\0';
        } else if (strncmp(line, "PORT=", 5) == 0) {
            int p = atoi(line + 5);
            if (p >= 1 && p <= 65535) scanner_port = p;
        } else if (strncmp(line, "MODEL=", 6) == 0) {
            strncpy(scanner_make_model, line + 6, sizeof(scanner_make_model) - 1);
            scanner_make_model[sizeof(scanner_make_model) - 1] = '\0';
        } else if (strncmp(line, "SOURCE=", 7) == 0) {
            idx = find_label_index(source_values, 2, line + 7);
            if (idx >= 0) source_index = idx;
        } else if (strncmp(line, "COLORMODE=", 10) == 0) {
            idx = find_label_index(color_all_values, COLOR_MODE_COUNT, line + 10);
            if (idx >= 0) color_index = idx;
        } else if (strncmp(line, "RESOLUTION=", 11) == 0) {
            int r = atoi(line + 11);
            for (idx = 0; idx < DPI_GUI_COUNT; idx++) {
                if (dpi_gui_values[idx] == r) { dpi_index = idx; break; }
            }
        } else if (strncmp(line, "FORMAT=", 7) == 0) {
            idx = find_label_index(format_mimes, 3, line + 7);
            if (idx >= 0) format_index = idx;
        } else if (strncmp(line, "PAGESIZE=", 9) == 0) {
            for (idx = 0; idx < 4; idx++) {
                if (strcmp((char *)size_labels[idx], line + 9) == 0) { size_index = idx; break; }
            }
        } else if (strncmp(line, "SAVEPATH=", 9) == 0) {
            strncpy(savepath_buffer, line + 9, sizeof(savepath_buffer) - 1);
            savepath_buffer[sizeof(savepath_buffer) - 1] = '\0';
        }
    }
    Close(file);

    if (scanner_host[0]) {
        strncpy(discovered[0].ip, scanner_host, sizeof(discovered[0].ip) - 1);
        discovered[0].ip[sizeof(discovered[0].ip) - 1] = '\0';
        if (scanner_make_model[0]) {
            snprintf(discovered[0].label, sizeof(discovered[0].label),
                     "%s (%s)", scanner_host, scanner_make_model);
        } else {
            snprintf(discovered[0].label, sizeof(discovered[0].label),
                     "%s (saved)", scanner_host);
        }
        discovered_count = 1;
    }
}

/* --------------------------------------------------------------------
 * LAN scanner discovery (mDNS / Bonjour / AirScan)
 *
 * Same shape as MintPRINT's mDNS printer discovery: a hand-built DNS PTR
 * query with the "QU" unicast-response bit set, so this never needs to
 * join the 224.0.0.251 multicast group to see replies. Deliberately does
 * not decode SRV/TXT records - a reply's source address is enough to
 * populate the picker; ScannerCapabilities after selection supplies the
 * real details. Queries both _uscan._tcp (eSCL/HTTP, the common case)
 * and _uscans._tcp (eSCL/HTTPS - discovered here, but not yet reachable,
 * see docs/ARCHITECTURE.md).
 * ----------------------------------------------------------------- */

static BOOL discovery_ip_seen(int count, const char *ip) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(discovered[i].ip, ip) == 0) return TRUE;
    }
    return FALSE;
}

static int build_mdns_ptr_query(unsigned char *buf, int buf_size, const char **labels) {
    static const unsigned char header[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    int off, i;

    if (buf_size < 40) return 0;

    memcpy(buf, header, sizeof(header));
    off = sizeof(header);

    for (i = 0; labels[i]; i++) {
        int len = (int)strlen(labels[i]);
        buf[off++] = (unsigned char)len;
        memcpy(buf + off, labels[i], len);
        off += len;
    }
    buf[off++] = 0x00;

    buf[off++] = 0x00; buf[off++] = 0x0C; /* QTYPE = PTR */
    buf[off++] = 0x80; buf[off++] = 0x01; /* QCLASS = IN, QU bit set */

    return off;
}

static int mdns_discover_scanners(int count_io, int max_results, const char **labels,
                                   const char *tag) {
    int sockfd;
    struct sockaddr_in dest;
    unsigned char query[64];
    int query_len;
    char *buf;
    int count = count_io;
    int poll_num;
    const int max_polls = 10; /* ~500ms per poll => ~5s total scan time */

    if (count >= max_results) return count;

    query_len = build_mdns_ptr_query(query, sizeof(query), labels);
    if (query_len <= 0) return count;

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        printf("Discovery: could not create mDNS socket\n");
        return count;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(5353);
    dest.sin_addr.s_addr = inet_addr((STRPTR)"224.0.0.251");

    if (sendto(sockfd, (char *)query, query_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        printf("Discovery: mDNS send failed (no route to 224.0.0.251?)\n");
        CloseSocket(sockfd);
        return count;
    }

    buf = malloc(1024);
    if (!buf) { CloseSocket(sockfd); return count; }

    for (poll_num = 0; poll_num < max_polls && count < max_results; poll_num++) {
        fd_set readfds;
        struct timeval tv;
        long ready;
        struct sockaddr_in from;
        socklen_t fromlen;
        ssize_t received;

        drain_gui_events();

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        ready = WaitSelect(sockfd + 1, &readfds, NULL, NULL, &tv, NULL);
        if (ready <= 0) continue;

        fromlen = sizeof(from);
        memset(&from, 0, sizeof(from));
        received = recvfrom(sockfd, buf, 1023, 0, (struct sockaddr *)&from, &fromlen);
        if (received < 12 || from.sin_port != htons(5353)) continue;
        if (buf[6] == 0 && buf[7] == 0) continue; /* ANCOUNT == 0 */

        {
            char ipstr[16];
            const unsigned char *b = (const unsigned char *)&from.sin_addr;

            snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (b[0] == 127) continue;

            if (!discovery_ip_seen(count, ipstr)) {
                strncpy(discovered[count].ip, ipstr, sizeof(discovered[count].ip) - 1);
                discovered[count].ip[sizeof(discovered[count].ip) - 1] = '\0';
                snprintf(discovered[count].label, sizeof(discovered[count].label),
                         "%s (%s)", ipstr, tag);
                printf("Discovery: found %s\n", discovered[count].label);
                count++;
            }
        }
    }

    free(buf);
    CloseSocket(sockfd);
    return count;
}

/* Extracts the substring between <tag ...> and </tag> (namespace prefix
   included, e.g. "scan:Platen") into out. Leaves out empty if the
   element isn't found - callers fall back to scanning the whole
   document in that case, since some scanners' capabilities responses
   don't split Platen/Adf into separate elements at all. */
static void extract_source_block(const char *xml, const char *tag, char *out, int out_size) {
    char open_tag[32], close_tag[32];
    const char *start, *body, *end;

    out[0] = '\0';
    snprintf(open_tag, sizeof(open_tag), "<%s", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    start = strstr(xml, open_tag);
    if (!start) return;
    body = strchr(start, '>');
    if (!body) return;
    body++;

    end = strstr(body, close_tag);
    if (!end) return;

    {
        int len = (int)(end - body);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, body, len);
        out[len] = '\0';
    }
}

/* Scrapes every "...XResolution>NNN</..." out of a (source-scoped, see
   extract_source_block) ScannerCapabilities fragment into a sorted,
   deduplicated list. Returns the count found (0 if none - the scanner
   doesn't advertise a discrete list at all, e.g. only a resolution
   range). */
static int scrape_dpi_values(const char *xml, int *out) {
    int count = 0;
    int i;
    const char *p = xml;

    while ((p = strstr(p, "XResolution>")) != NULL) {
        int v;
        p += 13; /* strlen("XResolution>") */
        v = atoi(p);
        if (v > 0) {
            int dup = 0;
            for (i = 0; i < count; i++) if (out[i] == v) { dup = 1; break; }
            if (!dup && count < MAX_DPI_OPTIONS) out[count++] = v;
        }
    }

    for (i = 1; i < count; i++) {
        int key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > key) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return count;
}

/* Returns the DPI to actually put in the eSCL request: the selected
   dpi_gui_values[dpi_index] as-is if it's known to be supported for the
   *currently selected Source* (or if we don't know - no query has run
   yet), otherwise the closest value that source actually advertised.
   Prints a status line when it substitutes something, so a silent
   mismatch (the original bug report) is never silent again. Falls back
   to scraping the whole capabilities document if the current Source
   isn't broken out as its own element in it. */
static int resolve_dpi(void) {
    static char scoped[8192];
    int requested = dpi_gui_values[dpi_index];
    int supported[MAX_DPI_OPTIONS];
    int count, i, best;

    if (!have_capabilities) return requested;

    extract_source_block(capabilities_xml, source_capability_tags[source_index], scoped, sizeof(scoped));
    count = scrape_dpi_values(scoped[0] ? scoped : capabilities_xml, supported);
    if (count == 0) return requested;

    for (i = 0; i < count; i++) if (supported[i] == requested) return requested;

    best = supported[0];
    for (i = 1; i < count; i++) {
        if (abs(supported[i] - requested) < abs(best - requested)) best = supported[i];
    }
    printf("%d DPI not supported for %s - using %d DPI\n",
           requested, (char *)source_labels[source_index], best);
    return best;
}

/* Same idea as scrape_dpi_values but for ColorMode: records which of
   RGB24/Grayscale8/BlackAndWhite1 appear anywhere in a (source-scoped)
   capabilities fragment. Returns the count found. */
static int scrape_color_values(const char *xml, int *out_master) {
    int count = 0;
    int i;

    for (i = 0; i < COLOR_MODE_COUNT; i++) {
        if (strstr(xml, color_all_values[i])) out_master[count++] = i;
    }
    return count;
}

/* Returns the eSCL ColorMode value to actually put in the request: the
   selected mode as-is if it's supported for the currently selected
   Source (or unknown - no query has run yet), otherwise the first mode
   that source does support. Prints a status line when it substitutes
   something. */
static const char *resolve_color_value(void) {
    static char scoped[8192];
    int supported[COLOR_MODE_COUNT];
    int count, i;

    if (!have_capabilities) return color_all_values[color_index];

    extract_source_block(capabilities_xml, source_capability_tags[source_index], scoped, sizeof(scoped));
    count = scrape_color_values(scoped[0] ? scoped : capabilities_xml, supported);
    if (count == 0) return color_all_values[color_index];

    for (i = 0; i < count; i++) if (supported[i] == color_index) return color_all_values[color_index];

    printf("%s not supported for %s - using %s\n",
           (char *)color_all_labels[color_index], (char *)source_labels[source_index],
           (char *)color_all_labels[supported[0]]);
    return color_all_values[supported[0]];
}

static void rebuild_scanner_dropdown(void) {
    int i;
    static const char *unset_label = "(none found)";

    for (i = 0; i < discovered_count && i < MAX_DISCOVERY_RESULTS; i++) {
        strncpy(scanner_label_storage[i], discovered[i].label, sizeof(scanner_label_storage[i]) - 1);
        scanner_label_storage[i][sizeof(scanner_label_storage[i]) - 1] = '\0';
        scanner_label_ptrs[i] = (STRPTR)scanner_label_storage[i];
    }
    if (discovered_count == 0) {
        strncpy(scanner_label_storage[0], unset_label, sizeof(scanner_label_storage[0]) - 1);
        scanner_label_ptrs[0] = (STRPTR)scanner_label_storage[0];
        scanner_label_ptrs[1] = NULL;
    } else {
        scanner_label_ptrs[discovered_count] = NULL;
    }
    scanner_dropdown_labels = scanner_label_ptrs;
}

static void select_discovered_scanner(int idx) {
    if (idx < 0 || idx >= discovered_count) return;
    strncpy(scanner_host, discovered[idx].ip, sizeof(scanner_host) - 1);
    scanner_host[sizeof(scanner_host) - 1] = '\0';
    scanner_port = 80;
}

/* Parses "host" or "host:port" out of the IP entry field. Returns FALSE
   (leaving the outputs untouched) if there's no host to parse. */
static BOOL parse_host_port(const char *input, char *host_out, int host_size, int *port_out) {
    const char *colon;
    int len;

    while (*input == ' ') input++;
    if (!*input) return FALSE;

    colon = strchr(input, ':');
    len = colon ? (int)(colon - input) : (int)strlen(input);
    if (len <= 0 || len >= host_size) return FALSE;

    memcpy(host_out, input, len);
    host_out[len] = '\0';

    *port_out = 80;
    if (colon && colon[1]) {
        int p = atoi(colon + 1);
        if (p >= 1 && p <= 65535) *port_out = p;
    }
    return TRUE;
}

/* Adds (or updates) a manually-entered scanner in discovered[] - same
   list mDNS discovery populates - so it shows up in the Scanner dropdown
   and can be picked up by Save Config like a discovered one, then makes
   it the active scanner. Returns its index, or -1 if the list is full
   and this host wasn't already in it. */
static int add_or_select_manual_ip(const char *host, int port) {
    int i, idx = -1;

    for (i = 0; i < discovered_count; i++) {
        if (strcmp(discovered[i].ip, host) == 0) { idx = i; break; }
    }
    if (idx < 0 && discovered_count < MAX_DISCOVERY_RESULTS) {
        idx = discovered_count++;
        strncpy(discovered[idx].ip, host, sizeof(discovered[idx].ip) - 1);
        discovered[idx].ip[sizeof(discovered[idx].ip) - 1] = '\0';
    }
    if (idx >= 0) {
        snprintf(discovered[idx].label, sizeof(discovered[idx].label), "%s (manual)", host);
    }

    strncpy(scanner_host, host, sizeof(scanner_host) - 1);
    scanner_host[sizeof(scanner_host) - 1] = '\0';
    scanner_port = port;

    rebuild_scanner_dropdown();
    return idx;
}

/* --------------------------------------------------------------------
 * HTTP over bsdsocket - shared by ScannerCapabilities, ScanJobs and
 * NextDocument. Text requests/responses only; see download_next_document
 * for the streaming-to-file variant used for the scanned image itself.
 * ----------------------------------------------------------------- */

static int http_connect(const char *ip, int port) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct timeval timeout;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) return -1;

    timeout.tv_sec = 8;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
        CloseSocket(sockfd);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr((STRPTR)ip);
    if (serv_addr.sin_addr.s_addr == INADDR_NONE) {
        CloseSocket(sockfd);
        return -1;
    }

    drain_gui_events();
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        CloseSocket(sockfd);
        return -1;
    }
    return sockfd;
}

/* recv() bounded by an explicit WaitSelect rather than trusting
   SO_RCVTIMEO alone - not every bsdsocket.library stack honours socket
   receive timeouts reliably (the same caution MintPRINT's own mDNS code
   applies to UDP sockets). Everything here runs on one thread, so a
   recv() that never returns freezes the whole GUI - this is what a
   scanner that stalls mid-response, or holds the connection open
   without sending more data, was doing before. Returns like recv():
   >0 bytes, 0 on orderly close, <0 on error or timeout. */
static ssize_t recv_timeout(int sockfd, char *buf, int len, int timeout_secs) {
    fd_set readfds;
    struct timeval tv;
    long ready;

    drain_gui_events();
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = timeout_secs;
    tv.tv_usec = 0;
    ready = WaitSelect(sockfd + 1, &readfds, NULL, NULL, &tv, NULL);
    if (ready <= 0) return -1;
    return recv(sockfd, buf, len, 0);
}

/* GET request. On success returns the HTTP status code and leaves the
   response body (headers stripped) in response[]. */
static int http_get(const char *ip, int port, const char *path,
                     char *response, int maxlen) {
    int sockfd;
    char header[512];
    int total = 0;
    char *body;
    int status = -1;

    sockfd = http_connect(ip, port);
    if (sockfd < 0) {
        printf("Connect failed: %s:%d\n", ip, port);
        return -1;
    }

    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip);

    if (send(sockfd, header, strlen(header), 0) < 0) {
        CloseSocket(sockfd);
        return -1;
    }

    while (total < maxlen - 1) {
        ssize_t received = recv_timeout(sockfd, response + total, maxlen - 1 - total, 8);
        if (received > 0) {
            total += received;
            response[total] = '\0';
        } else {
            break; /* connection closed, or a real error - either way, done */
        }
    }
    CloseSocket(sockfd);

    if (total == 0) return -1;
    response[total] = '\0';

    sscanf(response, "HTTP/%*d.%*d %d", &status);
    body = strstr(response, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(response, body, strlen(body) + 1);
    }
    return status;
}

/* POST with an XML body. On success (201 Created for ScanJobs) fills in
   location[] from the response's Location header. */
static int http_post_xml(const char *ip, int port, const char *path,
                          const char *body_in, char *location, int location_size) {
    int sockfd;
    char header[512];
    char response[2048];
    int total = 0;
    int status = -1;
    char *loc_hdr;

    sockfd = http_connect(ip, port);
    if (sockfd < 0) {
        printf("Connect failed: %s:%d\n", ip, port);
        return -1;
    }

    snprintf(header, sizeof(header),
             "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: text/xml\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n",
             path, ip, (int)strlen(body_in));

    if (send(sockfd, header, strlen(header), 0) < 0 ||
        send(sockfd, (char *)body_in, strlen(body_in), 0) < 0) {
        CloseSocket(sockfd);
        return -1;
    }

    while (total < (int)sizeof(response) - 1) {
        ssize_t received = recv_timeout(sockfd, response + total, sizeof(response) - 1 - total, 8);
        if (received > 0) {
            total += received;
            response[total] = '\0';
            if (strstr(response, "\r\n\r\n")) break; /* don't need the body here */
        } else {
            break;
        }
    }
    CloseSocket(sockfd);

    if (total == 0) return -1;
    response[total] = '\0';
    sscanf(response, "HTTP/%*d.%*d %d", &status);

    location[0] = '\0';
    loc_hdr = strstr(response, "Location:");
    if (!loc_hdr) loc_hdr = strstr(response, "location:");
    if (loc_hdr) {
        char *end;
        loc_hdr += 9;
        while (*loc_hdr == ' ') loc_hdr++;
        end = strstr(loc_hdr, "\r\n");
        if (end) {
            int len = (int)(end - loc_hdr);
            if (len >= location_size) len = location_size - 1;
            memcpy(location, loc_hdr, len);
            location[len] = '\0';
        }
    }
    return status;
}

/* Best-effort job cleanup - failure here is not fatal, most scanners
   expire an unclaimed job on their own after a timeout. */
static void http_delete(const char *ip, int port, const char *path) {
    int sockfd;
    char header[512];

    sockfd = http_connect(ip, port);
    if (sockfd < 0) return;

    /* Fire-and-forget: we don't care about the response, and some
       scanners never reply to DELETE or never close the connection
       afterwards - reading a response here previously meant recv()
       could block the whole (single-threaded) GUI indefinitely. Just
       send the request and close; the scanner will time the job out
       on its own if this doesn't land. */
    snprintf(header, sizeof(header),
             "DELETE %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip);
    send(sockfd, header, strlen(header), 0);
    CloseSocket(sockfd);
}

/* Strips a possible "http://host[:port]" prefix off a Location header
   value, leaving just the path - scanners are inconsistent about
   returning an absolute URL vs. a bare path here. */
static void normalize_location_path(const char *location, char *path_out, int path_size) {
    const char *p = location;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        p = strchr(p, '/');
        if (!p) p = "/";
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        p = strchr(p, '/');
        if (!p) p = "/";
    }
    strncpy(path_out, p, path_size - 1);
    path_out[path_size - 1] = '\0';
}

/* Chunked-transfer decoder for NextDocument responses. Several real
   eSCL scanners (Brother's included) send "Transfer-Encoding: chunked"
   instead of Content-Length here - without decoding it, the hex chunk-
   size lines and CRLF framing end up written into the file as if they
   were image bytes, producing a file that's roughly the right size but
   corrupt (the symptom that led to this). */
enum ChunkState { CS_SIZE, CS_SIZE_CR, CS_DATA, CS_DATA_CR, CS_DATA_LF, CS_DONE };

struct ChunkDecoder {
    enum ChunkState state;
    long remaining;
    char sizebuf[16];
    int sizelen;
};

static void chunk_decoder_init(struct ChunkDecoder *cd) {
    cd->state = CS_SIZE;
    cd->remaining = 0;
    cd->sizelen = 0;
}

/* Feeds len raw (still chunk-encoded) bytes through the decoder, writing
   only the actual payload - not the size lines or chunk CRLFs - to
   outfile. Returns TRUE once the terminating 0-length chunk has been
   seen. Trailer headers after that chunk (rare - most servers just send
   "0\r\n\r\n") are not parsed; anything after it is ignored. */
static BOOL chunk_decoder_feed(struct ChunkDecoder *cd, const char *buf, int len,
                                BPTR outfile, long *written) {
    int i = 0;

    while (i < len) {
        switch (cd->state) {
        case CS_SIZE:
            if (buf[i] == '\r') {
                cd->state = CS_SIZE_CR;
                i++;
            } else if (buf[i] == ';') {
                while (i < len && buf[i] != '\r') i++;
            } else {
                if (cd->sizelen < (int)sizeof(cd->sizebuf) - 1) {
                    cd->sizebuf[cd->sizelen++] = buf[i];
                }
                i++;
            }
            break;

        case CS_SIZE_CR:
            if (buf[i] != '\n') { cd->state = CS_DONE; return TRUE; }
            cd->sizebuf[cd->sizelen] = '\0';
            cd->remaining = strtol(cd->sizebuf, NULL, 16);
            cd->sizelen = 0;
            i++;
            if (cd->remaining == 0) { cd->state = CS_DONE; return TRUE; }
            cd->state = CS_DATA;
            break;

        case CS_DATA: {
            int avail = len - i;
            int take = (cd->remaining < (long)avail) ? (int)cd->remaining : avail;
            if (take > 0) {
                Write(outfile, (APTR)(buf + i), take);
                *written += take;
                cd->remaining -= take;
                i += take;
            }
            if (cd->remaining == 0) cd->state = CS_DATA_CR;
            break;
        }

        case CS_DATA_CR:
            cd->state = CS_DATA_LF;
            i++;
            break;

        case CS_DATA_LF:
            cd->state = CS_SIZE;
            i++;
            break;

        case CS_DONE:
            return TRUE;
        }
    }
    return cd->state == CS_DONE;
}

/* Downloads NextDocument straight to disk, splitting the HTTP header
   from the binary body as bytes arrive rather than buffering the whole
   image in memory first. Honours Content-Length or Transfer-Encoding:
   chunked when the server sends either; otherwise reads until the
   connection closes. */
static BOOL download_next_document(const char *ip, int port, const char *job_path,
                                    const char *save_path, long *bytes_out) {
    int sockfd;
    char header[512];
    char chunk[4096];
    char headbuf[2048];
    int headlen = 0;
    BOOL header_done = FALSE;
    BOOL is_chunked = FALSE;
    BOOL chunked_done = FALSE;
    struct ChunkDecoder cd;
    long content_length = -1;
    long written = 0;
    BPTR outfile;
    int status = -1;

    sockfd = http_connect(ip, port);
    if (sockfd < 0) {
        printf("Connect failed for NextDocument\n");
        return FALSE;
    }

    snprintf(header, sizeof(header),
             "GET %s/NextDocument HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             job_path, ip);
    if (send(sockfd, header, strlen(header), 0) < 0) {
        CloseSocket(sockfd);
        return FALSE;
    }

    outfile = Open((CONST_STRPTR)save_path, MODE_NEWFILE);
    if (!outfile) {
        printf("Could not create %s\n", save_path);
        CloseSocket(sockfd);
        return FALSE;
    }

    for (;;) {
        ssize_t received;
        received = recv_timeout(sockfd, chunk, sizeof(chunk), 8);
        if (received <= 0) break;

        if (!header_done) {
            int copy = (int)received;
            char *term;

            if (headlen + copy > (int)sizeof(headbuf) - 1) {
                copy = (int)sizeof(headbuf) - 1 - headlen;
            }
            memcpy(headbuf + headlen, chunk, copy);
            headlen += copy;
            headbuf[headlen] = '\0';

            term = strstr(headbuf, "\r\n\r\n");
            if (term) {
                char *cl;
                int header_bytes = (int)(term - headbuf) + 4;
                int body_in_chunk = (int)received - (header_bytes - (headlen - copy));

                sscanf(headbuf, "HTTP/%*d.%*d %d", &status);
                cl = strstr(headbuf, "Content-Length:");
                if (!cl) cl = strstr(headbuf, "content-length:");
                if (cl) {
                    cl += 15; /* strlen("Content-Length:") */
                    while (*cl == ' ') cl++;
                    content_length = atol(cl);
                }
                {
                    char *te = strstr(headbuf, "Transfer-Encoding:");
                    if (!te) te = strstr(headbuf, "transfer-encoding:");
                    if (te) {
                        char line[64];
                        char *lend = strstr(te, "\r\n");
                        int linelen = lend ? (int)(lend - te) : (int)strlen(te);
                        if (linelen >= (int)sizeof(line)) linelen = sizeof(line) - 1;
                        memcpy(line, te, linelen);
                        line[linelen] = '\0';
                        if (strstr(line, "chunked") || strstr(line, "Chunked")) {
                            is_chunked = TRUE;
                            chunk_decoder_init(&cd);
                        }
                    }
                }

                header_done = TRUE;

                if (status != 200) {
                    printf("NextDocument returned status %d\n", status);
                    Close(outfile);
                    DeleteFile((CONST_STRPTR)save_path);
                    CloseSocket(sockfd);
                    return FALSE;
                }

                if (body_in_chunk > 0) {
                    if (is_chunked) {
                        chunked_done = chunk_decoder_feed(&cd, chunk + (received - body_in_chunk),
                                                           body_in_chunk, outfile, &written);
                    } else {
                        Write(outfile, chunk + (received - body_in_chunk), body_in_chunk);
                        written += body_in_chunk;
                    }
                }
            }
        } else if (is_chunked) {
            chunked_done = chunk_decoder_feed(&cd, chunk, received, outfile, &written);
        } else {
            Write(outfile, chunk, received);
            written += received;
        }

        if (is_chunked) {
            if (chunked_done) break;
        } else if (content_length >= 0 && written >= content_length) {
            break;
        }
    }

    Close(outfile);
    CloseSocket(sockfd);

    if (!header_done) {
        printf("NextDocument: no response\n");
        return FALSE;
    }

    *bytes_out = written;
    return TRUE;
}

/* --------------------------------------------------------------------
 * eSCL protocol calls
 * ----------------------------------------------------------------- */

static void query_capabilities(const char *ip, int port) {
    int status;
    char *tag, *end;

    printf("Querying capabilities: %s:%d\n", ip, port);
    have_capabilities = FALSE;
    status = http_get(ip, port, "/eSCL/ScannerCapabilities", capabilities_xml, sizeof(capabilities_xml));
    if (status != 200) {
        printf("ScannerCapabilities failed (status %d)\n", status);
        capabilities_xml[0] = '\0';
        return;
    }

    scanner_make_model[0] = '\0';
    tag = strstr(capabilities_xml, "MakeAndModel>");
    if (tag) {
        tag += 13;
        end = strstr(tag, "</");
        if (end) {
            int len = (int)(end - tag);
            if (len >= (int)sizeof(scanner_make_model)) len = sizeof(scanner_make_model) - 1;
            memcpy(scanner_make_model, tag, len);
            scanner_make_model[len] = '\0';
        }
    }

    have_capabilities = TRUE;
    if (scanner_make_model[0]) {
        printf("Found: %s\n", scanner_make_model);
    } else {
        printf("Capabilities OK (model name not found in response)\n");
    }
}

static void build_scan_settings_xml(char *buf, int buf_size) {
    int dpi = resolve_dpi();
    const char *color_value = resolve_color_value();

    /* Always shown, not just on substitution - if the scanner still
       ignores this, the request itself is the next thing to check, not
       which value we picked. */
    printf("Requesting: %d DPI, %s, %s\n", dpi, color_value, source_values[source_index]);

    /* pwg:ScanRegion's schema sequence puts ContentRegionUnits first,
       before Height/Width/XOffset/YOffset - it was missing entirely
       here before. Confirmed by testing that a real scanner can accept
       the job (still returns 201/a valid image) while silently
       ignoring every other requested field (resolution, colour mode)
       and falling back to its own defaults for all of them - consistent
       with a strict/fragile firmware parser failing region validation
       and discarding the rest of the document rather than just that
       one field. scan:Intent is likewise commonly present in working
       real-world requests and cheap to include. */
    snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" "
        "xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">\n"
        "<pwg:Version>2.0</pwg:Version>\n"
        "<scan:Intent>Document</scan:Intent>\n"
        "<pwg:ScanRegions>\n"
        "<pwg:ScanRegion>\n"
        "<pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>\n"
        "<pwg:Height>%d</pwg:Height>\n"
        "<pwg:Width>%d</pwg:Width>\n"
        "<pwg:XOffset>0</pwg:XOffset>\n"
        "<pwg:YOffset>0</pwg:YOffset>\n"
        "</pwg:ScanRegion>\n"
        "</pwg:ScanRegions>\n"
        "<pwg:InputSource>%s</pwg:InputSource>\n"
        "<scan:ColorMode>%s</scan:ColorMode>\n"
        "<scan:XResolution>%d</scan:XResolution>\n"
        "<scan:YResolution>%d</scan:YResolution>\n"
        "<pwg:DocumentFormat>%s</pwg:DocumentFormat>\n"
        "</scan:ScanSettings>\n",
        size_height_300[size_index], size_width_300[size_index],
        source_values[source_index], color_value,
        dpi, dpi,
        format_mimes[format_index]);
}

static void do_scan(void) {
    char xml[1024];
    char location[256];
    char job_path[256];
    long bytes_written = 0;
    int status;

    if (!scanner_host[0]) {
        printf("No scanner selected - use Discover first\n");
        return;
    }

    build_scan_settings_xml(xml, sizeof(xml));

    printf("Starting scan job...\n");
    status = http_post_xml(scanner_host, scanner_port, "/eSCL/ScanJobs", xml,
                            location, sizeof(location));
    if (status != 201 || !location[0]) {
        printf("ScanJobs failed (status %d)\n", status);
        return;
    }

    normalize_location_path(location, job_path, sizeof(job_path));
    printf("Job created: %s\n", job_path);

    printf("Downloading to %s...\n", savepath_buffer);
    if (download_next_document(scanner_host, scanner_port, job_path,
                                savepath_buffer, &bytes_written)) {
        printf("Saved %ld bytes to %s\n", bytes_written, savepath_buffer);
    } else {
        printf("Scan failed\n");
    }

    http_delete(scanner_host, scanner_port, job_path);
}

static void do_discover(void) {
    printf("Searching LAN for scanners (mDNS)...\n");
    discovered_count = 0;

    {
        static const char *uscan_labels[] = { "_uscan", "_tcp", "local", NULL };
        static const char *uscans_labels[] = { "_uscans", "_tcp", "local", NULL };
        discovered_count = mdns_discover_scanners(discovered_count, MAX_DISCOVERY_RESULTS,
                                                   uscan_labels, "eSCL");
        discovered_count = mdns_discover_scanners(discovered_count, MAX_DISCOVERY_RESULTS,
                                                   uscans_labels, "eSCL/TLS");
    }

    rebuild_scanner_dropdown();

    if (discovered_count > 0) {
        select_discovered_scanner(0);
        query_capabilities(scanner_host, scanner_port);
    } else {
        printf("No scanners found\n");
    }
}

/* --------------------------------------------------------------------
 * GUI
 * ----------------------------------------------------------------- */

static struct Gadget *find_gadget_by_id(struct Window *win, int id);
static struct Gadget *find_model_gadget(struct Window *win);
static void sync_string_gadget(struct Window *win, int gadget_id, char *dest, int dest_size);

static struct Gadget *createAllGadgets(struct Gadget **glistptr, void *ivi, UWORD topborder) {
    struct NewGadget ng;
    struct Gadget *gad;

    gad = CreateContext(glistptr);
    if (!gad) return NULL;

    ng.ng_TextAttr = &Topaz80;
    ng.ng_VisualInfo = ivi;
    ng.ng_Flags = NG_HIGHLABEL;

    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge = 5 + topborder;
    ng.ng_Width = 250;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Scanner:";
    ng.ng_GadgetID = GAD_SCANNER_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)scanner_dropdown_labels,
        GTCY_Active, 0,
        TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 370;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"_Discover";
    ng.ng_GadgetID = GAD_DISCOVER_BUTTON;
    ng.ng_Flags = 0;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) return NULL;
    ng.ng_Flags = NG_HIGHLABEL;

    /* Manual entry, for setups (e.g. WinUAE) where mDNS multicast never
       reaches the Amiga - type an IP (or "ip:port") and Query it directly,
       no discovery needed. */
    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 250;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"_IP:";
    ng.ng_GadgetID = GAD_IP_STRING;
    gad = CreateGadget(STRING_KIND, gad, &ng,
        GTST_String, (ULONG)ip_entry_buffer,
        GTST_MaxChars, sizeof(ip_entry_buffer) - 1,
        GA_Immediate, TRUE,
        GT_Underscore, '_',
        GACT_RELVERIFY, TRUE,
        TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 370;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"_Query";
    ng.ng_GadgetID = GAD_QUERY_BUTTON;
    ng.ng_Flags = 0;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) return NULL;
    ng.ng_Flags = NG_HIGHLABEL;

    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 350;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Model:";
    ng.ng_GadgetID = GAD_MODEL_DISPLAY;
    gad = CreateGadget(STRING_KIND, gad, &ng,
        GTST_String, (ULONG)scanner_make_model,
        GTST_MaxChars, sizeof(scanner_make_model) - 1,
        GA_Disabled, TRUE,
        TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 150;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Source:";
    ng.ng_GadgetID = GAD_SOURCE_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)source_labels, GTCY_Active, source_index, TAG_DONE);
    if (!gad) return NULL;

    ng.ng_TopEdge += 20;
    ng.ng_GadgetText = (STRPTR)"Colour:";
    ng.ng_GadgetID = GAD_COLOR_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)color_all_labels, GTCY_Active, color_index, TAG_DONE);
    if (!gad) return NULL;

    ng.ng_TopEdge += 20;
    ng.ng_Width = 100;
    ng.ng_GadgetText = (STRPTR)"DPI:";
    ng.ng_GadgetID = GAD_DPI_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)dpi_gui_labels, GTCY_Active, dpi_index, TAG_DONE);
    if (!gad) return NULL;

    ng.ng_TopEdge += 20;
    ng.ng_GadgetText = (STRPTR)"Format:";
    ng.ng_GadgetID = GAD_FORMAT_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)format_labels, GTCY_Active, format_index, TAG_DONE);
    if (!gad) return NULL;

    ng.ng_TopEdge += 20;
    ng.ng_GadgetText = (STRPTR)"Size:";
    ng.ng_GadgetID = GAD_SIZE_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)size_labels, GTCY_Active, size_index, TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 360;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Save _to:";
    ng.ng_GadgetID = GAD_SAVEPATH_STRING;
    gad = CreateGadget(STRING_KIND, gad, &ng,
        GTST_String, (ULONG)savepath_buffer,
        GTST_MaxChars, sizeof(savepath_buffer) - 1,
        GA_Immediate, TRUE,
        GT_Underscore, '_',
        GACT_RELVERIFY, TRUE,
        TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge += 30;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"_Scan";
    ng.ng_GadgetID = GAD_SCAN_BUTTON;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 190;
    ng.ng_Width = 110;
    ng.ng_GadgetText = (STRPTR)"S_ave Config";
    ng.ng_GadgetID = GAD_SAVE_BUTTON;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 370;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"_Exit";
    ng.ng_GadgetID = GAD_EXIT_BUTTON;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) return NULL;

    return gad;
}

static void process_window_events(struct Window *win) {
    struct IntuiMessage *imsg;
    ULONG imsgClass;
    struct Gadget *gad;
    BOOL terminated = FALSE;

    while (!terminated) {
        Wait(1L << win->UserPort->mp_SigBit);

        imsg = GT_GetIMsg(win->UserPort);
        while (!terminated && imsg) {
            gad = (struct Gadget *)imsg->IAddress;
            imsgClass = imsg->Class;

            GT_ReplyIMsg(imsg);

            switch (imsgClass) {
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                        case GAD_SCANNER_DROPDOWN: {
                            if (!operation_in_progress) {
                                ULONG selected = 0;
                                struct Gadget *mg;
                                operation_in_progress = TRUE;
                                GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&selected, TAG_DONE);
                                select_discovered_scanner((int)selected);
                                if (scanner_host[0]) query_capabilities(scanner_host, scanner_port);
                                mg = find_model_gadget(win);
                                if (mg) {
                                    GT_SetGadgetAttrs(mg, win, NULL,
                                                       GTST_String, (ULONG)scanner_make_model, TAG_DONE);
                                }
                                operation_in_progress = FALSE;
                            }
                            break;
                        }
                        case GAD_DISCOVER_BUTTON:
                            if (!operation_in_progress) {
                                struct Gadget *sg, *mg;
                                operation_in_progress = TRUE;
                                do_discover();
                                sg = find_gadget_by_id(win, GAD_SCANNER_DROPDOWN);
                                if (sg) {
                                    GT_SetGadgetAttrs(sg, win, NULL,
                                        GTCY_Labels, (ULONG)scanner_dropdown_labels,
                                        GTCY_Active, 0, TAG_DONE);
                                }
                                mg = find_model_gadget(win);
                                if (mg) {
                                    GT_SetGadgetAttrs(mg, win, NULL,
                                                       GTST_String, (ULONG)scanner_make_model, TAG_DONE);
                                }
                                operation_in_progress = FALSE;
                            }
                            break;
                        case GAD_QUERY_BUTTON:
                            if (!operation_in_progress) {
                                char host[64];
                                int port;
                                operation_in_progress = TRUE;
                                sync_string_gadget(win, GAD_IP_STRING, ip_entry_buffer, sizeof(ip_entry_buffer));
                                if (parse_host_port(ip_entry_buffer, host, sizeof(host), &port)) {
                                    int idx = add_or_select_manual_ip(host, port);
                                    struct Gadget *sg, *mg;
                                    query_capabilities(scanner_host, scanner_port);
                                    sg = find_gadget_by_id(win, GAD_SCANNER_DROPDOWN);
                                    if (sg) {
                                        GT_SetGadgetAttrs(sg, win, NULL,
                                            GTCY_Labels, (ULONG)scanner_dropdown_labels,
                                            GTCY_Active, (ULONG)(idx >= 0 ? idx : 0), TAG_DONE);
                                    }
                                    mg = find_model_gadget(win);
                                    if (mg) {
                                        GT_SetGadgetAttrs(mg, win, NULL,
                                                           GTST_String, (ULONG)scanner_make_model, TAG_DONE);
                                    }
                                } else {
                                    printf("Enter a scanner IP first\n");
                                }
                                operation_in_progress = FALSE;
                            }
                            break;
                        case GAD_IP_STRING:
                            sync_string_gadget(win, GAD_IP_STRING, ip_entry_buffer, sizeof(ip_entry_buffer));
                            break;
                        case GAD_SOURCE_DROPDOWN: {
                            ULONG v = 0;
                            GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
                            source_index = (int)v;
                            break;
                        }
                        case GAD_COLOR_DROPDOWN: {
                            ULONG v = 0;
                            GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
                            color_index = (int)v;
                            break;
                        }
                        case GAD_DPI_DROPDOWN: {
                            ULONG v = 0;
                            GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
                            dpi_index = (int)v;
                            break;
                        }
                        case GAD_FORMAT_DROPDOWN: {
                            ULONG v = 0;
                            char *dot, *slash, *colon, *base;
                            struct Gadget *sg;

                            GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
                            format_index = (int)v;

                            /* Keep "Save to:" in sync with the chosen
                               format's extension - otherwise it's easy to
                               pick PNG/PDF and still write a .jpg filename.
                               Only replace what's after the rightmost path
                               separator, so a '.' in a directory name isn't
                               mistaken for the extension. */
                            slash = strrchr(savepath_buffer, '/');
                            colon = strrchr(savepath_buffer, ':');
                            if (slash && colon) base = (slash > colon) ? slash : colon;
                            else if (slash) base = slash;
                            else if (colon) base = colon;
                            else base = NULL;

                            dot = strrchr(base ? base : savepath_buffer, '.');
                            if (dot) {
                                snprintf(dot + 1, sizeof(savepath_buffer) - (size_t)(dot + 1 - savepath_buffer),
                                         "%s", format_extensions[format_index]);
                            } else {
                                size_t len = strlen(savepath_buffer);
                                snprintf(savepath_buffer + len, sizeof(savepath_buffer) - len,
                                         ".%s", format_extensions[format_index]);
                            }

                            sg = find_gadget_by_id(win, GAD_SAVEPATH_STRING);
                            if (sg) {
                                GT_SetGadgetAttrs(sg, win, NULL, GTST_String, (ULONG)savepath_buffer, TAG_DONE);
                            }
                            break;
                        }
                        case GAD_SIZE_DROPDOWN: {
                            ULONG v = 0;
                            GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
                            size_index = (int)v;
                            break;
                        }
                        case GAD_SAVEPATH_STRING:
                            sync_string_gadget(win, GAD_SAVEPATH_STRING, savepath_buffer, sizeof(savepath_buffer));
                            break;
                        case GAD_SCAN_BUTTON:
                            if (!operation_in_progress) {
                                operation_in_progress = TRUE;
                                sync_string_gadget(win, GAD_SAVEPATH_STRING, savepath_buffer, sizeof(savepath_buffer));
                                do_scan();
                                operation_in_progress = FALSE;
                            }
                            break;
                        case GAD_SAVE_BUTTON:
                            sync_string_gadget(win, GAD_SAVEPATH_STRING, savepath_buffer, sizeof(savepath_buffer));
                            if (save_config()) {
                                printf("Saved to ENV(ARC):MintSCAN/Unit0\n");
                            } else {
                                printf("Save failed\n");
                            }
                            break;
                        case GAD_EXIT_BUTTON:
                            terminated = TRUE;
                            break;
                    }
                    break;

                case IDCMP_CLOSEWINDOW:
                    terminated = TRUE;
                    break;

                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    redraw_output_box();
                    break;
            }

            imsg = terminated ? NULL : GT_GetIMsg(win->UserPort);
        }
    }
}

static struct Gadget *find_gadget_by_id(struct Window *win, int id) {
    struct Gadget *g = glist;
    (void)win;
    while (g && g->GadgetID != id) g = g->NextGadget;
    return g;
}

static struct Gadget *find_model_gadget(struct Window *win) {
    return find_gadget_by_id(win, GAD_MODEL_DISPLAY);
}

/* GTST_String only sets a string gadget's INITIAL text at creation time -
   GadTools keeps its own internal edit buffer after that, so live typing
   never touches the buffer we passed in. GT_GetGadgetAttrs(..., GTST_String,
   ...) is the documented way to read back what's actually in the gadget
   right now; call this before using anything the user may have typed. */
static void sync_string_gadget(struct Window *win, int gadget_id, char *dest, int dest_size) {
    struct Gadget *g = find_gadget_by_id(win, gadget_id);
    STRPTR live = NULL;

    if (!g) return;
    GT_GetGadgetAttrs(g, win, NULL, GTST_String, (ULONG)&live, TAG_DONE);
    if (live) {
        strncpy(dest, (char *)live, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

int main(void) {
    UWORD topborder;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (!IntuitionBase) return 1;

    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!GfxBase) { CloseLibrary((struct Library *)IntuitionBase); return 1; }

    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!GadToolsBase) {
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 0);
    if (!SocketBase) {
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    font = OpenFont(&Topaz60);
    if (!font) {
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    screen = LockPubScreen(NULL);
    if (!screen) {
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    vi = GetVisualInfo(screen, TAG_DONE);
    if (!vi) {
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    topborder = screen->WBorTop + (screen->Font->ta_YSize + 1);

    load_config();
    rebuild_scanner_dropdown();
    if (scanner_host[0]) {
        if (scanner_port != 80) {
            snprintf(ip_entry_buffer, sizeof(ip_entry_buffer), "%s:%d", scanner_host, scanner_port);
        } else {
            strncpy(ip_entry_buffer, scanner_host, sizeof(ip_entry_buffer) - 1);
            ip_entry_buffer[sizeof(ip_entry_buffer) - 1] = '\0';
        }
    }

    if (!createAllGadgets(&glist, vi, topborder)) {
        FreeVisualInfo(vi);
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    window = OpenWindowTags(NULL,
        WA_Title, (ULONG)"MintSCAN",
        WA_Gadgets, (ULONG)glist,
        WA_AutoAdjust, TRUE,
        WA_Width, 480,
        WA_MinWidth, 480,
        WA_InnerHeight, 340,
        WA_MinHeight, 340,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_SizeGadget, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_NewLookMenus, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | STRINGIDCMP | BUTTONIDCMP | CYCLEIDCMP,
        WA_PubScreen, (ULONG)screen,
        TAG_DONE);

    if (!window) {
        FreeGadgets(glist);
        FreeVisualInfo(vi);
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    custom_printf("CLEAR");
    GT_RefreshWindow(window, NULL);

    if (scanner_host[0]) {
        printf("Loaded saved scanner: %s\n", scanner_host);
    } else {
        printf("Click Discover to find scanners on the LAN\n");
    }

    process_window_events(window);

    if (window) { CloseWindow(window); window = NULL; }
    if (glist) { FreeGadgets(glist); glist = NULL; }

    if (vi) { FreeVisualInfo(vi); vi = NULL; }
    if (screen) { UnlockPubScreen(NULL, screen); screen = NULL; }
    if (font) { CloseFont(font); font = NULL; }

    CloseLibrary(SocketBase);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);

    return 0;
}

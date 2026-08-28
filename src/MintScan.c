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
#include <proto/asl.h>
#include <libraries/asl.h>
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
#include <sys/ioctl.h> /* FIONBIO - connect_with_timeout() */
#include <errno.h>     /* EINPROGRESS/EWOULDBLOCK - connect_with_timeout() */

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
#define GAD_UNIT_DROPDOWN    15
#define GAD_STATUS_DISPLAY   16
#define GAD_BROWSE_BUTTON    17

#define MINTSCAN_VERSION "1.2.1"

/* Saved scanner profiles: ENV(ARC):MintSCAN/Unit0 .. Unit(MAX_UNITS-1),
   switched via GAD_UNIT_DROPDOWN - same Unit0-7 idea as MintPRINT's
   printer profiles. Unlike MintPRINT there is no background driver with
   its own idea of which Unit is "live" - MintSCAN is the only reader of
   these files, so whichever Unit is currently loaded in the GUI is simply
   what Scan uses next; there is no separate Activate step. */
#define MAX_UNITS 8
#define UNIT_LABEL_LEN 48

#define MAX_DISCOVERY_RESULTS 16
#define MAX_DPI_OPTIONS 10
#define MAX_BUFFER    65536   /* capabilities / ScanJobs response scratch */
#define MAX_OUTPUT_LINES 8
#define MAX_OUTPUT_LINE_LENGTH 55
#define OUTPUT_TOP    259
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

/* The Unit currently loaded/shown in the GUI - see MAX_UNITS above. */
static int current_unit_index = 0;
static char unit_label_storage[MAX_UNITS][UNIT_LABEL_LEN];
static STRPTR unit_label_ptrs[MAX_UNITS + 1];
static STRPTR *unit_dropdown_labels = NULL;

/* The scanner currently selected in the GUI - may come from discovery,
   or from a saved profile loaded at startup before any Discover click. */
static char scanner_host[64] = "";
static int  scanner_port = 80;
static char scanner_make_model[96] = "";
static BOOL have_capabilities = FALSE;

/* Last-queried /eSCL/ScannerStatus pwg:State (Idle/Processing/Testing/
   Stopped/Down) - see query_scanner_status(). Shown live next to Model,
   the same idea as MintPRINT showing printer-state next to its ink/toner
   strip, and used to turn a bare ScanJobs failure status code into an
   actual reason ("scanner reports: Processing" instead of "status 503"). */
static char scanner_status_text[32] = "";

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
static const int size_height_300[] = { 3508, 3300, 4200, 4961 };

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
/* Not static: proto/ headers already declare these extern (non-static) -
   this is the actual definition, matching MintPRINT's own convention. */
struct Library *SocketBase = NULL;
struct Library *GadToolsBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
/* Optional, unlike the libraries above: the "Browse" file requester just
   doesn't offer itself (see GAD_BROWSE_BUTTON) if this is NULL, rather
   than the app refusing to start over a nice-to-have. */
struct Library *AslBase = NULL;
static BOOL operation_in_progress = FALSE;

static struct TextAttr Topaz80 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextAttr Topaz60 = { (STRPTR)"topaz.font", 6, 0, 0 };

/* status/progress output goes to the on-screen box, never a console -
   this may be launched from Workbench, where there is no console. */
static void custom_printf(const char *format, ...);
#define printf custom_printf

/* Forward declarations - definition order below follows the eSCL data
   flow (config -> discovery -> HTTP -> GUI), but the Unit-switching
   helpers in the config section need these GUI-section functions. */
static struct Gadget *find_gadget_by_id(struct Window *win, int id);
static struct Gadget *find_model_gadget(struct Window *win);
static void rebuild_scanner_dropdown(void);
static void refresh_status_gadget(void);

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

static void unit_config_path(int idx, BOOL envarc, char *out, int out_size) {
    snprintf(out, out_size, "%s:MintSCAN/Unit%d", envarc ? "ENVARC" : "ENV", idx);
}

static BOOL unit_file_exists(int idx) {
    BPTR lock;
    char path[64];

    unit_config_path(idx, FALSE, path, sizeof(path));
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock) {
        unit_config_path(idx, TRUE, path, sizeof(path));
        lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    }
    if (lock) { UnLock(lock); return TRUE; }
    return FALSE;
}

/* Peeks just the MODEL= line out of a saved Unit file, without disturbing
   any of the live GUI state - used to label the Unit dropdown entries. */
static void peek_unit_model(int idx, char *out, int out_size) {
    BPTR file;
    char path[64];
    char line[192];

    out[0] = '\0';
    unit_config_path(idx, FALSE, path, sizeof(path));
    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!file) {
        unit_config_path(idx, TRUE, path, sizeof(path));
        file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    }
    if (!file) return;

    while (FGets(file, (STRPTR)line, sizeof(line))) {
        trim_config_line(line);
        if (strncmp(line, "MODEL=", 6) == 0 && line[6]) {
            strncpy(out, line + 6, out_size - 1);
            out[out_size - 1] = '\0';
            break;
        }
    }
    Close(file);
}

static BOOL write_config_file(CONST_STRPTR filename, int idx) {
    BPTR file;
    char line[256];

    file = Open(filename, MODE_NEWFILE);
    if (!file) return FALSE;

    snprintf(line, sizeof(line), "# MintSCAN Unit%d - written by MintScan\n", idx);
    FPuts(file, (STRPTR)line);
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
    char env_path[64], envarc_path[64];

    if (!ensure_config_dir((CONST_STRPTR)"ENV:MintSCAN")) {
        printf("Could not create ENV:MintSCAN\n");
        return FALSE;
    }
    if (!ensure_config_dir((CONST_STRPTR)"ENVARC:MintSCAN")) {
        printf("Could not create ENVARC:MintSCAN\n");
        return FALSE;
    }

    unit_config_path(current_unit_index, FALSE, env_path, sizeof(env_path));
    unit_config_path(current_unit_index, TRUE, envarc_path, sizeof(envarc_path));
    env_ok = write_config_file((CONST_STRPTR)env_path, current_unit_index);
    envarc_ok = write_config_file((CONST_STRPTR)envarc_path, current_unit_index);
    return env_ok && envarc_ok;
}

/* Resets every saved field to MintScan's built-in defaults (the same ones
   the static initialisers above use) - called before loading a Unit so
   switching to an empty slot doesn't leave the previous Unit's fields
   lingering in the GUI. */
static void reset_unit_defaults(void) {
    scanner_host[0] = '\0';
    scanner_port = 80;
    scanner_make_model[0] = '\0';
    source_index = 0;
    color_index = 0;
    dpi_index = 3;
    format_index = 0;
    size_index = 0;
    strncpy(savepath_buffer, "RAM:scan001.jpg", sizeof(savepath_buffer) - 1);
    savepath_buffer[sizeof(savepath_buffer) - 1] = '\0';
}

/* Loads Unit%d (ENV: first, falling back to ENVARC:) into the live
   scanner_host/source_index/etc. globals, having reset them to defaults
   first. Returns TRUE if a saved file for that Unit was found, FALSE if
   it fell back to defaults (an empty/never-saved slot). Does not touch
   the GUI itself - see apply_unit_to_gadgets() for that. */
static BOOL load_unit_config(int idx) {
    BPTR file;
    char path[64];
    char line[256];
    int lidx;

    reset_unit_defaults();
    discovered_count = 0;

    unit_config_path(idx, FALSE, path, sizeof(path));
    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!file) {
        unit_config_path(idx, TRUE, path, sizeof(path));
        file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    }
    if (!file) return FALSE;

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
            lidx = find_label_index(source_values, 2, line + 7);
            if (lidx >= 0) source_index = lidx;
        } else if (strncmp(line, "COLORMODE=", 10) == 0) {
            lidx = find_label_index(color_all_values, COLOR_MODE_COUNT, line + 10);
            if (lidx >= 0) color_index = lidx;
        } else if (strncmp(line, "RESOLUTION=", 11) == 0) {
            int r = atoi(line + 11);
            for (lidx = 0; lidx < DPI_GUI_COUNT; lidx++) {
                if (dpi_gui_values[lidx] == r) { dpi_index = lidx; break; }
            }
        } else if (strncmp(line, "FORMAT=", 7) == 0) {
            lidx = find_label_index(format_mimes, 3, line + 7);
            if (lidx >= 0) format_index = lidx;
        } else if (strncmp(line, "PAGESIZE=", 9) == 0) {
            for (lidx = 0; lidx < 4; lidx++) {
                if (strcmp((char *)size_labels[lidx], line + 9) == 0) { size_index = lidx; break; }
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
        /* Just the IP, not "IP (Model)" - the Model field right below the
           Scanner dropdown already shows the model name, and the combined
           label was long enough (a real make/model easily runs past 20
           characters) to get clipped behind the Discover button in the
           cycle gadget's fixed-width box. */
        snprintf(discovered[0].label, sizeof(discovered[0].label),
                 "%s (saved)", scanner_host);
        discovered_count = 1;
    }
    return TRUE;
}

/* Rebuilds ip_entry_buffer (GAD_IP_STRING's content) from the currently
   loaded scanner_host/scanner_port - shared by startup and by switching
   the Unit dropdown, both of which load a new host into those globals
   and need the manual-entry field to reflect it. */
static void sync_ip_entry_buffer(void) {
    if (!scanner_host[0]) { ip_entry_buffer[0] = '\0'; return; }
    if (scanner_port != 80) {
        snprintf(ip_entry_buffer, sizeof(ip_entry_buffer), "%s:%d", scanner_host, scanner_port);
    } else {
        strncpy(ip_entry_buffer, scanner_host, sizeof(ip_entry_buffer) - 1);
        ip_entry_buffer[sizeof(ip_entry_buffer) - 1] = '\0';
    }
}

/* Rebuilds the Unit dropdown's labels from whatever is currently saved on
   disk for each slot ("0: Brother MFC-J6930DW", "1 (empty)", ...).
   Callable before the window exists (win == NULL) to seed the gadget's
   initial GTCY_Labels, or afterwards to refresh a live gadget - e.g.
   after Save, in case a freshly-queried model name just got written out. */
static void refresh_unit_dropdown(struct Window *win) {
    int i;

    for (i = 0; i < MAX_UNITS; i++) {
        char model[UNIT_LABEL_LEN];

        unit_label_ptrs[i] = (STRPTR)unit_label_storage[i];
        model[0] = '\0';

        if (i == current_unit_index && scanner_make_model[0]) {
            strncpy(model, scanner_make_model, sizeof(model) - 1);
            model[sizeof(model) - 1] = '\0';
        } else {
            peek_unit_model(i, model, sizeof(model));
        }

        /* The "Unit:" gadget label already says "Unit" - don't repeat it
           in every entry, a real model name badly needs the width. */
        if (model[0]) {
            snprintf(unit_label_storage[i], UNIT_LABEL_LEN, "%d: %s", i, model);
        } else if (unit_file_exists(i)) {
            snprintf(unit_label_storage[i], UNIT_LABEL_LEN, "%d", i);
        } else {
            snprintf(unit_label_storage[i], UNIT_LABEL_LEN, "%d (empty)", i);
        }
    }
    unit_label_ptrs[MAX_UNITS] = NULL;
    unit_dropdown_labels = unit_label_ptrs;

    if (win) {
        struct Gadget *g = find_gadget_by_id(win, GAD_UNIT_DROPDOWN);
        if (g) {
            GT_SetGadgetAttrs(g, win, NULL,
                               GTCY_Labels, (ULONG)unit_dropdown_labels,
                               GTCY_Active, (ULONG)current_unit_index,
                               TAG_DONE);
        }
    }
}

/* Pushes every loaded field (scanner_host/source_index/etc., set by
   load_unit_config()) into its on-screen gadget. Used after switching
   the Unit dropdown, since GadTools gadgets don't alias these globals
   for live redraws the way the string buffers get read back from. */
static void apply_unit_to_gadgets(struct Window *win) {
    struct Gadget *g;

    rebuild_scanner_dropdown();
    sync_ip_entry_buffer();

    if ((g = find_gadget_by_id(win, GAD_SCANNER_DROPDOWN))) {
        GT_SetGadgetAttrs(g, win, NULL,
            GTCY_Labels, (ULONG)scanner_dropdown_labels, GTCY_Active, 0, TAG_DONE);
    }
    if ((g = find_gadget_by_id(win, GAD_IP_STRING))) {
        GT_SetGadgetAttrs(g, win, NULL, GTST_String, (ULONG)ip_entry_buffer, TAG_DONE);
    }
    if ((g = find_model_gadget(win))) {
        GT_SetGadgetAttrs(g, win, NULL, GTTX_Text, (ULONG)scanner_make_model, TAG_DONE);
    }
    refresh_status_gadget();
    if ((g = find_gadget_by_id(win, GAD_SOURCE_DROPDOWN))) {
        GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)source_index, TAG_DONE);
    }
    if ((g = find_gadget_by_id(win, GAD_COLOR_DROPDOWN))) {
        GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)color_index, TAG_DONE);
    }
    if ((g = find_gadget_by_id(win, GAD_DPI_DROPDOWN))) {
        GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)dpi_index, TAG_DONE);
    }
    if ((g = find_gadget_by_id(win, GAD_FORMAT_DROPDOWN))) {
        GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)format_index, TAG_DONE);
    }
    if ((g = find_gadget_by_id(win, GAD_SIZE_DROPDOWN))) {
        GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)size_index, TAG_DONE);
    }
    if ((g = find_gadget_by_id(win, GAD_SAVEPATH_STRING))) {
        GT_SetGadgetAttrs(g, win, NULL, GTST_String, (ULONG)savepath_buffer, TAG_DONE);
    }
}

/* Reloads everything for current_unit_index: saved Unit%d config and the
   on-screen gadgets that show it. Used when switching the Unit dropdown. */
static void reload_current_unit(struct Window *win) {
    have_capabilities = FALSE;
    capabilities_xml[0] = '\0';
    scanner_status_text[0] = '\0';

    if (load_unit_config(current_unit_index)) {
        printf("Loaded Unit%d\n", current_unit_index);
    } else {
        printf("Unit%d is empty - using defaults\n", current_unit_index);
    }

    refresh_unit_dropdown(win);
    if (win) apply_unit_to_gadgets(win);
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

/* Scrapes every exact <scan:XResolution>NNN</scan:XResolution> out of a
   (source-scoped, see extract_source_block) ScannerCapabilities fragment
   into a sorted, deduplicated list. Returns the count found (0 if none -
   the scanner doesn't advertise a discrete list at all, e.g. only a
   resolution range). Match the complete element name so
   MaxOpticalXResolution can't be mistaken for a selectable resolution. */
static int scrape_dpi_values(const char *xml, int *out) {
    int count = 0;
    int i;
    const char *p = xml;
    const char *tag = "<scan:XResolution>";

    while ((p = strstr(p, tag)) != NULL) {
        int v;
        p += strlen(tag);
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

/* connect() bounded by an explicit non-blocking connect + WaitSelect
   rather than however long the stack's own blocking connect() feels
   like taking - same technique as MintPRINT's mp_connect_with_timeout.
   Returns like connect(): 0 on success, -1 on failure/timeout. */
static int connect_with_timeout(int sockfd, struct sockaddr_in *addr, int timeout_secs) {
    long nonblock = 1, block = 0;
    int rc, connect_errno;

    if (IoctlSocket(sockfd, FIONBIO, (char *)&nonblock) < 0) {
        /* Non-blocking mode unavailable on this stack - fall back to a
           plain blocking connect rather than failing outright. */
        return connect(sockfd, (struct sockaddr *)addr, sizeof(*addr));
    }

    rc = connect(sockfd, (struct sockaddr *)addr, sizeof(*addr));
    /* bsdsocket.library does not update the standard C errno global -
       Errno() is its own error state, read back via proto/bsdsocket.h.
       Which value it returns for "in progress" on a non-blocking connect
       isn't standardised across stacks, so both are accepted. */
    connect_errno = (rc < 0) ? Errno() : 0;

    if (rc < 0 && (connect_errno == EINPROGRESS || connect_errno == EWOULDBLOCK)) {
        int elapsed_ms = 0;
        const int chunk_ms = 250;
        int outcome = -2; /* -2 = still waiting, -1 = failed, 0 = connected */

        while (outcome == -2 && elapsed_ms < timeout_secs * 1000) {
            fd_set wfds, efds;
            struct timeval tv;
            long ready;

            drain_gui_events();

            FD_ZERO(&wfds); FD_SET(sockfd, &wfds);
            FD_ZERO(&efds); FD_SET(sockfd, &efds);
            tv.tv_sec = 0;
            tv.tv_usec = chunk_ms * 1000;

            ready = WaitSelect(sockfd + 1, NULL, &wfds, &efds, &tv, NULL);
            if (ready > 0 && (FD_ISSET(sockfd, &wfds) || FD_ISSET(sockfd, &efds))) {
                int so_err = 0;
                socklen_t optlen = sizeof(so_err);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&so_err, &optlen) == 0) {
                    outcome = (so_err == 0) ? 0 : -1;
                } else {
                    /* getsockopt(SO_ERROR) unsupported on this stack -
                       write-readiness alone is the best signal available
                       that the attempt resolved, so trust it. */
                    outcome = 0;
                }
            }
            elapsed_ms += chunk_ms;
        }

        if (outcome == -2) outcome = -1; /* timed out */
        rc = outcome;
    }

    IoctlSocket(sockfd, FIONBIO, (char *)&block);
    return rc;
}

static int http_connect_once(const char *ip, int port) {
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
    if (connect_with_timeout(sockfd, &serv_addr, 8) < 0) {
        CloseSocket(sockfd);
        return -1;
    }
    return sockfd;
}

/* Some Wi-Fi eSCL scanners (matching MintPRINT's HP OfficeJet/Envy IPP
   findings) let their radio drop into power-save between jobs. mDNS
   discovery still gets a reply because the radio wakes for multicast
   traffic, but the first real TCP SYN afterwards can be slow enough to
   blow past a single connect attempt, even though the same scanner
   answers almost immediately once its radio is awake. One retry before
   giving up covers that case; a genuinely dead endpoint fails fast via
   ECONNREFUSED/host-unreachable well inside the connect timeout either
   way, so the retry costs it little. */
static int http_connect(const char *ip, int port) {
    int sockfd = http_connect_once(ip, port);
    if (sockfd < 0) sockfd = http_connect_once(ip, port);
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

/* send()'s write-side counterpart to recv_timeout() - MintPRINT's own
   ipp_client had exactly this gap once: connect() and recv() bounded,
   send() still a plain blocking call. A scanner that accepts a
   connection and then stops draining its TCP receive window mid-request
   (not just mid-response) would block send() forever, freezing this
   single-threaded GUI the same way an unbounded recv() did. Loops
   because a large write (the ScanSettings XML body) can legitimately
   return a short count rather than sending it all in one call. Returns
   the total bytes sent, or -1 on timeout/error (matching recv_timeout's
   convention) - never a partial-success count. */
static ssize_t send_timeout(int sockfd, const char *buf, int len, int timeout_secs) {
    int total = 0;

    while (total < len) {
        fd_set writefds;
        struct timeval tv;
        long ready;
        ssize_t sent;

        drain_gui_events();
        FD_ZERO(&writefds);
        FD_SET(sockfd, &writefds);
        tv.tv_sec = timeout_secs;
        tv.tv_usec = 0;
        ready = WaitSelect(sockfd + 1, NULL, &writefds, NULL, &tv, NULL);
        if (ready <= 0) return -1;

        sent = send(sockfd, (char *)(buf + total), len - total, 0);
        if (sent <= 0) return -1;
        total += (int)sent;
    }
    return total;
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

    if (send_timeout(sockfd, header, (int)strlen(header), 8) < 0) {
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

    if (send_timeout(sockfd, header, (int)strlen(header), 8) < 0 ||
        send_timeout(sockfd, body_in, (int)strlen(body_in), 8) < 0) {
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
    send_timeout(sockfd, header, (int)strlen(header), 8);
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
    if (send_timeout(sockfd, header, (int)strlen(header), 8) < 0) {
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

/* Pushes scanner_status_text into the live Status gadget, if the window
   is open yet. find_gadget_by_id() ignores its win argument (it always
   walks the global glist), so this is safe to call from do_scan(), which
   has no window parameter of its own - it uses the global window. */
static void refresh_status_gadget(void) {
    struct Gadget *g = find_gadget_by_id(window, GAD_STATUS_DISPLAY);
    if (g) {
        GT_SetGadgetAttrs(g, window, NULL, GTTX_Text, (ULONG)scanner_status_text, TAG_DONE);
    }
}

/* GET /eSCL/ScannerStatus and record just the top-level device state
   (eSCL's pwg:State: Idle/Processing/Testing/Stopped/Down) - not the
   per-job state further down the same document, which needs a job UUID
   to correlate and isn't tracked here. Not fatal if this fails or the
   element isn't present - ScannerStatus support is optional in some
   firmware and its absence shouldn't block scanning. */
static void query_scanner_status(const char *ip, int port) {
    char response[2048];
    int status;
    char *tag, *end;

    scanner_status_text[0] = '\0';
    status = http_get(ip, port, "/eSCL/ScannerStatus", response, sizeof(response));
    if (status == 200) {
        tag = strstr(response, "<pwg:State>");
        if (tag) {
            tag += 11;
            end = strstr(tag, "</");
            if (end) {
                int len = (int)(end - tag);
                if (len >= (int)sizeof(scanner_status_text)) len = sizeof(scanner_status_text) - 1;
                memcpy(scanner_status_text, tag, len);
                scanner_status_text[len] = '\0';
            }
        }
    }
    refresh_status_gadget();
}

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

    query_scanner_status(ip, port);
    if (scanner_status_text[0]) {
        printf("Scanner status: %s\n", scanner_status_text);
    }
}

/* sane-airscan records DocumentFormatExt support per source and only
   emits scan:DocumentFormatExt when that source advertises it. Some
   scanner firmware is permissive enough to return 201 Created for a
   request it doesn't fully understand, then silently performs a default
   scan instead, so don't send the extension blindly. */
static BOOL source_uses_document_format_ext(void) {
    static char scoped[8192];
    const char *scope;

    if (!have_capabilities) return FALSE;

    scoped[0] = '\0';
    extract_source_block(capabilities_xml, source_capability_tags[source_index],
                         scoped, sizeof(scoped));
    scope = scoped[0] ? scoped : capabilities_xml;

    return strstr(scope, "DocumentFormatExt") != NULL;
}

static void build_scan_settings_xml(char *buf, int buf_size) {
    int dpi = resolve_dpi();
    const char *mime = format_mimes[format_index];
    const char *color_value = resolve_color_value();
    BOOL use_document_format_ext = source_uses_document_format_ext();
    char format_ext[128];

    /* BlackAndWhite1 (1-bit) comes back corrupted on at least one real
       scanner - not a JPEG-only problem: a real-hardware report showed
       the same narrow, diagonally-sheared garbage in PNG output too,
       which rules out "JPEG can't encode 1-bit" as the cause (PNG
       genuinely can). Two structurally unrelated encoders breaking
       identically points at the scanner's own 1-bit raster capture/
       packing, upstream of whichever format wraps it - not something
       fixable from the request side. Substitute Grayscale8 across every
       format until a scanner is confirmed to actually produce a clean
       BlackAndWhite1 image; a real 1-bit encode is smaller/starker than
       Grayscale, but wrong output isn't a usable tradeoff for that. See
       docs/ARCHITECTURE.md for how to help narrow this down further. */
    if (strcmp(color_value, "BlackAndWhite1") == 0) {
        printf("Black & White scans corrupted on this hardware - using Grayscale instead\n");
        color_value = "Grayscale8";
    }

    format_ext[0] = '\0';
    if (use_document_format_ext) {
        snprintf(format_ext, sizeof(format_ext),
                 "<scan:DocumentFormatExt>%s</scan:DocumentFormatExt>", mime);
    }

    /* Always shown, not just on substitution - if the scanner still
       ignores this, the request itself is the next thing to check, not
       which value we picked. */
    printf("Requesting: %d DPI, %s, %s\n", dpi, color_value, source_values[source_index]);

    /* IMPORTANT: keep this XML compact. Brother MFC-J6930DW firmware
       accepts pretty-printed ScanSettings with HTTP 201 but silently
       falls back to a 200-DPI default. sane-airscan uses compact XML. */
    snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<scan:ScanSettings xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\" "
        "xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\">"
        "<pwg:Version>2.0</pwg:Version>"
        "<pwg:ScanRegions>"
        "<pwg:ScanRegion>"
        "<pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>"
        "<pwg:XOffset>0</pwg:XOffset>"
        "<pwg:YOffset>0</pwg:YOffset>"
        "<pwg:Width>%d</pwg:Width>"
        "<pwg:Height>%d</pwg:Height>"
        "</pwg:ScanRegion>"
        "</pwg:ScanRegions>"
        "<pwg:InputSource>%s</pwg:InputSource>"
        "<scan:ColorMode>%s</scan:ColorMode>"
        "<pwg:DocumentFormat>%s</pwg:DocumentFormat>"
        "%s"
        "<scan:XResolution>%d</scan:XResolution>"
        "<scan:YResolution>%d</scan:YResolution>"
        "</scan:ScanSettings>",
        size_width_300[size_index], size_height_300[size_index],
        source_values[source_index], color_value, mime, format_ext,
        dpi, dpi);
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
        query_scanner_status(scanner_host, scanner_port);
        if (scanner_status_text[0]) {
            printf("ScanJobs failed (status %d) - scanner reports: %s\n", status, scanner_status_text);
        } else {
            printf("ScanJobs failed (status %d)\n", status);
        }
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
    ng.ng_Width = 360;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Unit:";
    ng.ng_GadgetID = GAD_UNIT_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)unit_dropdown_labels,
        GTCY_Active, (ULONG)current_unit_index,
        TAG_DONE);
    if (!gad) return NULL;

    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
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
    /* Not "IP" alone: parse_host_port()'s result only ever reaches
       inet_addr() (see http_connect_once()), so a hostname is never
       actually resolved - same accurate-labeling fix MintPRINT applied
       to its own "Printer IP/Host" field once it realised the same
       thing about its own inet_addr()-only IP field. */
    ng.ng_GadgetText = (STRPTR)"_IPv4:";
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

    /* Model and Status are read-only display, not editable fields - a
       plain TEXT_KIND label (like MintPRINT's own Printer Model gadget)
       instead of a disabled STRING_KIND, which this NDK/theme renders as
       a hatched/greyed-out box that reads as "unavailable" rather than
       "here's the answer". GT_SetGadgetAttrs's GTTX_Text updates a live
       TEXT_KIND gadget's text the same way GTST_String updates a live
       STRING_KIND one. */
    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 350;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Model:";
    ng.ng_GadgetID = GAD_MODEL_DISPLAY;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
        GTTX_Text, (ULONG)scanner_make_model,
        GTTX_Justification, GTJ_LEFT,
        TAG_DONE);
    if (!gad) return NULL;

    /* Live /eSCL/ScannerStatus device state (Idle/Processing/.../Down),
       refreshed by refresh_status_gadget() whenever query_scanner_status()
       runs - including once at startup for a saved Unit's scanner, so
       this isn't blank until the first Discover/Query click. */
    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 200;
    ng.ng_GadgetText = (STRPTR)"Status:";
    ng.ng_GadgetID = GAD_STATUS_DISPLAY;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
        GTTX_Text, (ULONG)scanner_status_text,
        GTTX_Justification, GTJ_LEFT,
        TAG_DONE);
    if (!gad) return NULL;

    /* Source/Colour stay in a left column; DPI/Format/Size move to a
       right column sharing the same rows - mirrors MintPRINT's own
       paired CYCLE_KIND columns (e.g. Printer Engine/Debug beside
       Quality/DPI) instead of stacking five dropdowns in one column
       with the whole right half of the window sitting empty. */
    {
        UWORD row1 = ng.ng_TopEdge + 20; /* Source | DPI */
        UWORD row2 = row1 + 20;          /* Colour | Format */
        UWORD row3 = row2 + 20;          /* (left blank) | Size */

        ng.ng_LeftEdge = 100;
        ng.ng_TopEdge = row1;
        ng.ng_Width = 150;
        ng.ng_Height = 12;
        ng.ng_GadgetText = (STRPTR)"Source:";
        ng.ng_GadgetID = GAD_SOURCE_DROPDOWN;
        gad = CreateGadget(CYCLE_KIND, gad, &ng,
            GTCY_Labels, (ULONG)source_labels, GTCY_Active, source_index, TAG_DONE);
        if (!gad) return NULL;

        ng.ng_LeftEdge = 330;
        ng.ng_Width = 130;
        ng.ng_GadgetText = (STRPTR)"DPI:";
        ng.ng_GadgetID = GAD_DPI_DROPDOWN;
        gad = CreateGadget(CYCLE_KIND, gad, &ng,
            GTCY_Labels, (ULONG)dpi_gui_labels, GTCY_Active, dpi_index, TAG_DONE);
        if (!gad) return NULL;

        ng.ng_LeftEdge = 100;
        ng.ng_TopEdge = row2;
        ng.ng_Width = 150;
        ng.ng_GadgetText = (STRPTR)"Colour:";
        ng.ng_GadgetID = GAD_COLOR_DROPDOWN;
        gad = CreateGadget(CYCLE_KIND, gad, &ng,
            GTCY_Labels, (ULONG)color_all_labels, GTCY_Active, color_index, TAG_DONE);
        if (!gad) return NULL;

        ng.ng_LeftEdge = 330;
        ng.ng_Width = 130;
        ng.ng_GadgetText = (STRPTR)"Format:";
        ng.ng_GadgetID = GAD_FORMAT_DROPDOWN;
        gad = CreateGadget(CYCLE_KIND, gad, &ng,
            GTCY_Labels, (ULONG)format_labels, GTCY_Active, format_index, TAG_DONE);
        if (!gad) return NULL;

        ng.ng_LeftEdge = 330;
        ng.ng_TopEdge = row3;
        ng.ng_Width = 130;
        ng.ng_GadgetText = (STRPTR)"Size:";
        ng.ng_GadgetID = GAD_SIZE_DROPDOWN;
        gad = CreateGadget(CYCLE_KIND, gad, &ng,
            GTCY_Labels, (ULONG)size_labels, GTCY_Active, size_index, TAG_DONE);
        if (!gad) return NULL;
    }

    /* Save-to path plus a "Browse" button that opens an ASL file
       requester (see GAD_BROWSE_BUTTON below) - typing a full AmigaDOS
       path by hand was the only option before. */
    ng.ng_LeftEdge = 100;
    ng.ng_TopEdge += 20;
    ng.ng_Width = 260;
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

    ng.ng_LeftEdge = 370;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"_Browse";
    ng.ng_GadgetID = GAD_BROWSE_BUTTON;
    ng.ng_Flags = 0;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) return NULL;
    ng.ng_Flags = NG_HIGHLABEL;

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

/* Opens an ASL file requester pre-seeded from the current Save-to path
   (FilePart() splits it into a starting drawer/file the same way DOS
   itself would), and on OK rebuilds savepath_buffer from what was picked
   via AddPart() (not naive string concatenation - AddPart() knows
   whether the drawer already ends in '/'/':' and only inserts a
   separator when it doesn't). Does nothing but report the fact if
   asl.library never opened - Browse is a convenience on top of typing a
   path by hand, not a requirement. */
static void do_browse_savepath(struct Window *win) {
    struct FileRequester *fr;
    char dir_buf[108];
    char file_buf[108];
    STRPTR leaf;
    int dirlen;
    struct Gadget *sg;

    if (!AslBase) {
        printf("asl.library not available - type a path instead\n");
        return;
    }

    leaf = FilePart((STRPTR)savepath_buffer);
    dirlen = (int)((char *)leaf - savepath_buffer);
    if (dirlen >= (int)sizeof(dir_buf)) dirlen = sizeof(dir_buf) - 1;
    memcpy(dir_buf, savepath_buffer, dirlen);
    dir_buf[dirlen] = '\0';
    strncpy(file_buf, (char *)leaf, sizeof(file_buf) - 1);
    file_buf[sizeof(file_buf) - 1] = '\0';

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Save scan to",
        ASLFR_InitialDrawer, (ULONG)dir_buf,
        ASLFR_InitialFile, (ULONG)file_buf,
        ASLFR_DoSaveMode, TRUE,
        TAG_DONE);
    if (!fr) {
        printf("Could not open file requester\n");
        return;
    }

    if (AslRequest(fr, NULL)) {
        strncpy(savepath_buffer, (char *)fr->rf_Dir, sizeof(savepath_buffer) - 1);
        savepath_buffer[sizeof(savepath_buffer) - 1] = '\0';
        AddPart((STRPTR)savepath_buffer, (STRPTR)fr->rf_File, sizeof(savepath_buffer));

        sg = find_gadget_by_id(win, GAD_SAVEPATH_STRING);
        if (sg) {
            GT_SetGadgetAttrs(sg, win, NULL, GTST_String, (ULONG)savepath_buffer, TAG_DONE);
        }
        printf("Save to: %s\n", savepath_buffer);
    }

    FreeAslRequest(fr);
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
                        case GAD_UNIT_DROPDOWN: {
                            if (!operation_in_progress) {
                                ULONG selected = 0;
                                operation_in_progress = TRUE;
                                GT_GetGadgetAttrs(gad, win, NULL, GTCY_Active, (ULONG)&selected, TAG_DONE);
                                current_unit_index = (int)selected;
                                reload_current_unit(win);
                                operation_in_progress = FALSE;
                            }
                            break;
                        }
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
                                                       GTTX_Text, (ULONG)scanner_make_model, TAG_DONE);
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
                                                       GTTX_Text, (ULONG)scanner_make_model, TAG_DONE);
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
                                                           GTTX_Text, (ULONG)scanner_make_model, TAG_DONE);
                                    }
                                } else {
                                    printf("Enter a scanner IPv4 address first\n");
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
                        case GAD_BROWSE_BUTTON:
                            if (!operation_in_progress) {
                                operation_in_progress = TRUE;
                                sync_string_gadget(win, GAD_SAVEPATH_STRING, savepath_buffer, sizeof(savepath_buffer));
                                do_browse_savepath(win);
                                operation_in_progress = FALSE;
                            }
                            break;
                        case GAD_SCAN_BUTTON:
                            if (!operation_in_progress) {
                                operation_in_progress = TRUE;
                                sync_string_gadget(win, GAD_SAVEPATH_STRING, savepath_buffer, sizeof(savepath_buffer));
                                /* operation_in_progress already blocks a
                                   second click from doing anything (clicks
                                   during a scan get drained/ignored by
                                   drain_gui_events()), but the button still
                                   looked clickable through the whole
                                   ScanJobs/NextDocument round trip - grey it
                                   out so it visibly reflects that. */
                                GT_SetGadgetAttrs(gad, win, NULL, GA_Disabled, TRUE, TAG_DONE);
                                do_scan();
                                GT_SetGadgetAttrs(gad, win, NULL, GA_Disabled, FALSE, TAG_DONE);
                                operation_in_progress = FALSE;
                            }
                            break;
                        case GAD_SAVE_BUTTON:
                            sync_string_gadget(win, GAD_SAVEPATH_STRING, savepath_buffer, sizeof(savepath_buffer));
                            if (save_config()) {
                                printf("Saved to ENV(ARC):MintSCAN/Unit%d\n", current_unit_index);
                                refresh_unit_dropdown(win);
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

    /* Optional - do_browse_savepath() just falls back to reporting it's
       unavailable rather than the whole app refusing to start over a
       "Browse" convenience button. */
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 37);

    font = OpenFont(&Topaz60);
    if (!font) {
        if (AslBase) CloseLibrary(AslBase);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    screen = LockPubScreen(NULL);
    if (!screen) {
        CloseFont(font);
        if (AslBase) CloseLibrary(AslBase);
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
        if (AslBase) CloseLibrary(AslBase);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    topborder = screen->WBorTop + (screen->Font->ta_YSize + 1);

    load_unit_config(current_unit_index);
    refresh_unit_dropdown(NULL);
    rebuild_scanner_dropdown();
    sync_ip_entry_buffer();

    if (!createAllGadgets(&glist, vi, topborder)) {
        FreeVisualInfo(vi);
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        if (AslBase) CloseLibrary(AslBase);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    window = OpenWindowTags(NULL,
        WA_Title, (ULONG)"MintSCAN v" MINTSCAN_VERSION,
        WA_Gadgets, (ULONG)glist,
        WA_AutoAdjust, TRUE,
        WA_Width, 480,
        WA_MinWidth, 480,
        WA_InnerHeight, 354,
        WA_MinHeight, 354,
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
        if (AslBase) CloseLibrary(AslBase);
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
        /* Show current status on load too, not just after the next
           Discover/Query click - the window and its Status gadget both
           exist by this point, and this is exactly the bounded,
           worst-case-16s call connect_with_timeout()/http_connect()
           already make everywhere else, not an unbounded startup probe. */
        query_scanner_status(scanner_host, scanner_port);
    } else {
        printf("Click Discover to find scanners on the LAN\n");
    }

    process_window_events(window);

    if (window) { CloseWindow(window); window = NULL; }
    if (glist) { FreeGadgets(glist); glist = NULL; }

    if (vi) { FreeVisualInfo(vi); vi = NULL; }
    if (screen) { UnlockPubScreen(NULL, screen); screen = NULL; }
    if (font) { CloseFont(font); font = NULL; }

    if (AslBase) CloseLibrary(AslBase);
    CloseLibrary(SocketBase);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);

    return 0;
}

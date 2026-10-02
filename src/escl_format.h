/* eSCL document-format capability checks. Host-testable without Amiga headers.
   Only exact, source-scoped MIME values count; a nearby MIME or a container
   named DocumentFormats must not make an unsupported format appear supported. */
#ifndef MINTSCAN_ESCL_FORMAT_H
#define MINTSCAN_ESCL_FORMAT_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The two eSCL capability elements have different namespace prefixes.
   Avoid a generic substring search: image/png must not match image/png2,
   image/png;... or a MIME advertised by an unrelated element. */
static int ms_escl_has_format_value(const char *xml, const char *tag,
                                    const char *mime)
{
    char open_tag[64], close_tag[64];
    const char *p;
    size_t want;

    if (!xml || !tag || !mime) return 0;
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    want = strlen(mime);
    p = xml;

    while ((p = strstr(p, open_tag)) != NULL) {
        const char *value = p + strlen(open_tag);
        const char *end = strstr(value, close_tag);
        if (!end) break;
        while (value < end && isspace((unsigned char)*value)) value++;
        while (end > value && isspace((unsigned char)end[-1])) end--;
        if ((size_t)(end - value) == want && memcmp(value, mime, want) == 0)
            return 1;
        p += strlen(open_tag);
    }
    return 0;
}

/* *known is false if this source does not advertise any format values.
   A legacy scanner with incomplete capabilities should not be blocked. */
static int ms_escl_format_supported(const char *xml, const char *mime,
                                    int *known)
{
    int has_pwg, has_ext;
    if (known) *known = 0;
    if (!xml || !mime) return 0;
    has_pwg = strstr(xml, "<pwg:DocumentFormat>") != NULL;
    has_ext = strstr(xml, "<scan:DocumentFormatExt>") != NULL;
    if (known) *known = has_pwg || has_ext;
    return ms_escl_has_format_value(xml, "pwg:DocumentFormat", mime) ||
           ms_escl_has_format_value(xml, "scan:DocumentFormatExt", mime);
}

#endif

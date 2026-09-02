#include "mdns_endpoint.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t put_name(unsigned char *p, size_t off, const char *name)
{
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t n = dot ? (size_t)(dot - s) : strlen(s);
        p[off++] = (unsigned char)n;
        memcpy(p + off, s, n);
        off += n;
        if (!dot) break;
        s = dot + 1;
    }
    p[off++] = 0;
    return off;
}

static void u16(unsigned char *p, size_t off, unsigned int v)
{
    p[off] = (unsigned char)(v >> 8);
    p[off + 1] = (unsigned char)v;
}

static size_t rr_hdr(unsigned char *p, size_t off, const char *owner,
                     unsigned int type, unsigned int rdlen)
{
    off = put_name(p, off, owner);
    u16(p, off, type); off += 2;
    u16(p, off, 1); off += 2;
    p[off++] = 0;
    p[off++] = 0;
    p[off++] = 0;
    p[off++] = 0;
    u16(p, off, rdlen); off += 2;
    return off;
}

int main(void)
{
    unsigned char p[1024];
    size_t off = 12, start, rdlen_pos, rstart;
    struct MSMdnsEndpoint ep;
    const char *instance = "Brother MFC-J6930DW._uscan._tcp.local";
    const char txt1[] = "rs=custom-eSCL";
    const char txt2[] = "ty=Brother MFC-J6930DW";

    memset(p, 0, sizeof(p));
    p[7] = 3; /* ANCOUNT */

    start = off;
    off = put_name(p, off, "_uscan._tcp.local");
    u16(p, off, 12); off += 2; u16(p, off, 1); off += 2;
    off += 4; rdlen_pos = off; off += 2; rstart = off;
    off = put_name(p, off, instance);
    u16(p, rdlen_pos, (unsigned int)(off - rstart));
    (void)start;

    off = rr_hdr(p, off, instance, 33, 6);
    p[off++] = 0;
    p[off++] = 0;
    p[off++] = 0;
    p[off++] = 0;
    u16(p, off, 8080); off += 2;

    off = put_name(p, off, instance);
    u16(p, off, 16); off += 2; u16(p, off, 1); off += 2;
    off += 4; rdlen_pos = off; off += 2; rstart = off;
    p[off++] = (unsigned char)strlen(txt1); memcpy(p + off, txt1, strlen(txt1)); off += strlen(txt1);
    p[off++] = (unsigned char)strlen(txt2); memcpy(p + off, txt2, strlen(txt2)); off += strlen(txt2);
    u16(p, rdlen_pos, (unsigned int)(off - rstart));

    memset(&ep, 0, sizeof(ep));
    assert(ms_mdns_parse_endpoint(p, off, &ep));
    assert(ep.is_escl);
    assert(ep.port == 8080);
    assert(strcmp(ep.path, "/custom-eSCL") == 0);
    assert(strcmp(ep.label, "Brother MFC-J6930DW") == 0);
    assert(strcmp(ep.instance, instance) == 0);
    puts("mDNS endpoint parser tests passed");
    return 0;
}

#include "mdns_endpoint.h"

#include <string.h>

#define MS_DNS_TYPE_PTR 12U
#define MS_DNS_TYPE_TXT 16U
#define MS_DNS_TYPE_SRV 33U

static unsigned short ms_u16(const unsigned char *p)
{
    return (unsigned short)(((unsigned short)p[0] << 8) | p[1]);
}

static int ms_ascii_equal_nocase(const char *a, const char *b)
{
    unsigned char ca, cb;
    if (!a || !b) return 0;
    while (*a && *b) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int ms_ascii_ends_nocase(const char *text, const char *suffix)
{
    size_t tl, sl;
    if (!text || !suffix) return 0;
    tl = strlen(text);
    sl = strlen(suffix);
    if (sl > tl) return 0;
    return ms_ascii_equal_nocase(text + tl - sl, suffix);
}

static void ms_copy(char *dst, size_t size, const char *src)
{
    size_t n;
    if (!dst || size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int ms_dns_name(const unsigned char *packet, size_t packet_len,
                       size_t *offset, char *out, size_t out_size)
{
    size_t pos, next, used;
    unsigned int jumps = 0;
    int jumped = 0;

    if (!packet || !offset || !out || out_size == 0) return 0;
    pos = *offset;
    next = pos;
    used = 0;
    out[0] = '\0';

    while (pos < packet_len) {
        unsigned char len = packet[pos];
        if (len == 0) {
            if (!jumped) next = pos + 1;
            out[used] = '\0';
            *offset = next;
            return 1;
        }
        if ((len & 0xc0U) == 0xc0U) {
            size_t ptr;
            if (pos + 1 >= packet_len) return 0;
            ptr = (size_t)(((unsigned int)(len & 0x3fU) << 8) | packet[pos + 1]);
            if (ptr >= packet_len || ++jumps > 24U) return 0;
            if (!jumped) { next = pos + 2; jumped = 1; }
            pos = ptr;
            continue;
        }
        if ((len & 0xc0U) != 0 || len > 63U) return 0;
        ++pos;
        if (pos + len > packet_len) return 0;
        if (used) {
            if (used + 1 >= out_size) return 0;
            out[used++] = '.';
        }
        if (used + len >= out_size) return 0;
        memcpy(out + used, packet + pos, len);
        used += len;
        pos += len;
        if (!jumped) next = pos;
    }
    return 0;
}

static int ms_name_relevant(const char *owner, const struct MSMdnsEndpoint *ep)
{
    if (!owner || !ep) return 0;
    if (ep->instance[0] && ms_ascii_equal_nocase(owner, ep->instance)) return 1;
    return ms_ascii_ends_nocase(owner, "._uscan._tcp.local") ||
           ms_ascii_equal_nocase(owner, "_uscan._tcp.local");
}

static void ms_txt(struct MSMdnsEndpoint *ep,
                   const unsigned char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t item_len, key_len, value_len;
        const unsigned char *item, *eq;
        char value[MS_MDNS_NAME_MAX];

        item_len = data[off++];
        if (item_len == 0) continue;
        if (off + item_len > len) break;
        item = data + off;
        eq = (const unsigned char *)memchr(item, '=', item_len);
        if (eq) {
            key_len = (size_t)(eq - item);
            value_len = item_len - key_len - 1;
            if (value_len >= sizeof(value)) value_len = sizeof(value) - 1;
            memcpy(value, eq + 1, value_len);
            value[value_len] = '\0';

            if (key_len == 2 && (item[0] == 'r' || item[0] == 'R') &&
                (item[1] == 's' || item[1] == 'S')) {
                if (value[0]) {
                    if (value[0] == '/') ms_copy(ep->path, sizeof(ep->path), value);
                    else {
                        ep->path[0] = '/';
                        ms_copy(ep->path + 1, sizeof(ep->path) - 1, value);
                    }
                }
            } else if (key_len == 2 && (item[0] == 't' || item[0] == 'T') &&
                       (item[1] == 'y' || item[1] == 'Y')) {
                if (value[0]) ms_copy(ep->label, sizeof(ep->label), value);
            }
        }
        off += item_len;
    }
}

int ms_mdns_parse_endpoint(const unsigned char *packet, size_t packet_len,
                           struct MSMdnsEndpoint *ep)
{
    unsigned int qd, an, ns, ar, total, i;
    size_t off;
    int relevant = 0;

    if (!packet || !ep || packet_len < 12) return 0;
    qd = ms_u16(packet + 4);
    an = ms_u16(packet + 6);
    ns = ms_u16(packet + 8);
    ar = ms_u16(packet + 10);
    total = an + ns + ar;
    off = 12;

    for (i = 0; i < qd; ++i) {
        char ignored[MS_MDNS_NAME_MAX];
        if (!ms_dns_name(packet, packet_len, &off, ignored, sizeof(ignored))) return 0;
        if (off + 4 > packet_len) return 0;
        off += 4;
    }

    for (i = 0; i < total; ++i) {
        char owner[MS_MDNS_NAME_MAX];
        unsigned int type;
        size_t rdata, end;
        unsigned int rdlen;

        if (!ms_dns_name(packet, packet_len, &off, owner, sizeof(owner))) return relevant;
        if (off + 10 > packet_len) return relevant;
        type = ms_u16(packet + off);
        rdlen = ms_u16(packet + off + 8);
        off += 10;
        rdata = off;
        end = off + rdlen;
        if (end > packet_len) return relevant;

        if (type == MS_DNS_TYPE_PTR &&
            ms_ascii_equal_nocase(owner, "_uscan._tcp.local")) {
            size_t p = rdata;
            char target[MS_MDNS_NAME_MAX];
            if (ms_dns_name(packet, packet_len, &p, target, sizeof(target))) {
                ms_copy(ep->instance, sizeof(ep->instance), target);
                ep->is_escl = 1;
                relevant = 1;
            }
        } else if (type == MS_DNS_TYPE_SRV && rdlen >= 6 &&
                   ms_name_relevant(owner, ep)) {
            ep->port = (int)ms_u16(packet + rdata + 4);
            ep->is_escl = 1;
            relevant = 1;
        } else if (type == MS_DNS_TYPE_TXT && ms_name_relevant(owner, ep)) {
            ms_txt(ep, packet + rdata, rdlen);
            ep->is_escl = 1;
            relevant = 1;
        }

        off = end;
    }
    return relevant;
}

#include "http_response.h"

#include <string.h>

static char ms_http_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int ms_http_equal_n(const char *a, const char *b, int len)
{
    int i;
    for (i = 0; i < len; ++i) {
        if (ms_http_lower(a[i]) != ms_http_lower(b[i])) return 0;
    }
    return 1;
}

static int ms_http_line_end(const char *buf, int start, int limit)
{
    int i;
    for (i = start; i + 1 < limit; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') return i;
    }
    return -1;
}

int ms_http_find_body(const char *buf, int len, int start)
{
    int i;
    if (!buf || len < 4 || start < 0 || start + 4 > len) return -1;
    for (i = start; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    return -1;
}

int ms_http_status(const char *buf, int len, int start)
{
    int i = start;
    if (!buf || start < 0 || start >= len) return 0;
    while (i < len && buf[i] != ' ') ++i;
    if (i + 4 > len) return 0;
    if (buf[i + 1] < '0' || buf[i + 1] > '9' ||
        buf[i + 2] < '0' || buf[i + 2] > '9' ||
        buf[i + 3] < '0' || buf[i + 3] > '9') return 0;
    return (buf[i + 1] - '0') * 100 +
           (buf[i + 2] - '0') * 10 + (buf[i + 3] - '0');
}

static int ms_http_header_value(const char *buf, int header_start,
                                int body_off, const char *header_name,
                                int *value_start, int *value_len)
{
    int name_len;
    int line;
    int line_end;
    int colon;
    int end;

    if (!buf || !header_name || header_start < 0 || body_off < header_start + 4)
        return 0;

    name_len = (int)strlen(header_name);
    line_end = ms_http_line_end(buf, header_start, body_off);
    if (line_end < 0) return 0;
    line = line_end + 2; /* Skip the status line. */
    end = body_off - 2;  /* Start of the empty line ending the header. */

    while (line < end) {
        int first;
        int last;

        line_end = ms_http_line_end(buf, line, body_off);
        if (line_end < 0 || line_end == line) break;

        colon = line;
        while (colon < line_end && buf[colon] != ':') ++colon;
        if (colon - line == name_len && colon < line_end &&
            ms_http_equal_n(buf + line, header_name, name_len)) {
            first = colon + 1;
            while (first < line_end && (buf[first] == ' ' || buf[first] == '\t'))
                ++first;
            last = line_end;
            while (last > first && (buf[last - 1] == ' ' || buf[last - 1] == '\t'))
                --last;
            if (value_start) *value_start = first;
            if (value_len) *value_len = last - first;
            return 1;
        }
        line = line_end + 2;
    }
    return 0;
}

int ms_http_copy_header(const char *buf, int header_start, int body_off,
                        const char *header_name, char *out, int out_size)
{
    int start;
    int len;
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';
    if (!ms_http_header_value(buf, header_start, body_off, header_name,
                              &start, &len))
        return 0;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, buf + start, (size_t)len);
    out[len] = '\0';
    return 1;
}

int ms_http_content_length(const char *buf, int header_start, int body_off)
{
    int start;
    int len;
    int i;
    int value = 0;

    if (!ms_http_header_value(buf, header_start, body_off, "Content-Length",
                              &start, &len) || len <= 0)
        return -1;

    for (i = 0; i < len; ++i) {
        int digit;
        if (buf[start + i] < '0' || buf[start + i] > '9') return -1;
        digit = buf[start + i] - '0';
        if (value > 25000000) return -1;
        value = value * 10 + digit;
    }
    return value;
}

int ms_http_header_has_token(const char *buf, int header_start, int body_off,
                             const char *header_name, const char *token)
{
    int start;
    int len;
    int token_len;
    int pos = 0;

    if (!token || !ms_http_header_value(buf, header_start, body_off,
                                        header_name, &start, &len))
        return 0;
    token_len = (int)strlen(token);

    while (pos < len) {
        int first;
        int last;
        while (pos < len && (buf[start + pos] == ' ' ||
                             buf[start + pos] == '\t' ||
                             buf[start + pos] == ',')) ++pos;
        first = pos;
        while (pos < len && buf[start + pos] != ',' &&
               buf[start + pos] != ';') ++pos;
        last = pos;
        while (last > first && (buf[start + last - 1] == ' ' ||
                                buf[start + last - 1] == '\t')) --last;
        if (last - first == token_len &&
            ms_http_equal_n(buf + start + first, token, token_len))
            return 1;
        while (pos < len && buf[start + pos] != ',') ++pos;
    }
    return 0;
}

static int ms_http_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = ms_http_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Returns 1 when the terminal zero chunk and trailer terminator are present,
 * 0 when more bytes are needed, and -1 for malformed chunk framing. */
int ms_http_chunked_complete(const char *body, int encoded_len)
{
    int src = 0;

    if (!body || encoded_len < 0) return -1;
    for (;;) {
        int line_end = ms_http_line_end(body, src, encoded_len);
        int pos;
        int digits = 0;
        unsigned long size = 0;

        if (line_end < 0) return 0;
        pos = src;
        while (pos < line_end && body[pos] != ';' &&
               body[pos] != ' ' && body[pos] != '\t') {
            int hex = ms_http_hex(body[pos]);
            if (hex < 0 || size > 0x0fffffffUL) return -1;
            size = size * 16UL + (unsigned long)hex;
            ++digits;
            ++pos;
        }
        if (!digits) return -1;
        src = line_end + 2;

        if (size == 0) {
            int i;
            if (src + 2 <= encoded_len && body[src] == '\r' && body[src + 1] == '\n')
                return 1;
            for (i = src; i + 3 < encoded_len; ++i) {
                if (body[i] == '\r' && body[i + 1] == '\n' &&
                    body[i + 2] == '\r' && body[i + 3] == '\n')
                    return 1;
            }
            return 0;
        }

        if (size > (unsigned long)(encoded_len - src)) return 0;
        src += (int)size;
        if (src + 2 > encoded_len) return 0;
        if (body[src] != '\r' || body[src + 1] != '\n') return -1;
        src += 2;
    }
}

/* Decodes a complete chunked body in place and returns its payload length. */
int ms_http_decode_chunked(char *body, int encoded_len)
{
    int src = 0;
    int dst = 0;

    if (ms_http_chunked_complete(body, encoded_len) != 1) return -1;
    for (;;) {
        int line_end = ms_http_line_end(body, src, encoded_len);
        int pos = src;
        unsigned long size = 0;

        while (pos < line_end && body[pos] != ';' &&
               body[pos] != ' ' && body[pos] != '\t') {
            size = size * 16UL + (unsigned long)ms_http_hex(body[pos]);
            ++pos;
        }
        src = line_end + 2;
        if (size == 0) return dst;
        memmove(body + dst, body + src, (size_t)size);
        dst += (int)size;
        src += (int)size + 2;
    }
}

int ms_http_final_body(char *buf, int len, int *http_status,
                       int *body_off, int *body_len)
{
    int header_start = 0;
    int body;
    int status;
    int length;

    if (!buf || len < 0) return -1;

    for (;;) {
        body = ms_http_find_body(buf, len, header_start);
        if (body < 0) return 0;
        status = ms_http_status(buf, len, header_start);
        if (status < 100 || status > 999) return -1;
        if (status < 200) {
            /* Interim responses such as 100 Continue have no body. The
             * next status line begins exactly where this header ended. */
            header_start = body;
            continue;
        }
        break;
    }

    if (ms_http_header_has_token(buf, header_start, body,
                                 "Transfer-Encoding", "chunked")) {
        int complete = ms_http_chunked_complete(buf + body, len - body);
        if (complete <= 0) return complete;
        length = ms_http_decode_chunked(buf + body, len - body);
        if (length < 0) return -1;
    } else {
        length = ms_http_content_length(buf, header_start, body);
        if (length >= 0) {
            if (len - body < length) return 0;
        } else {
            /* An unframed response is complete when the caller has read
             * through an orderly connection close. */
            length = len - body;
            if (length < 1) return 0;
        }
    }

    if (http_status) *http_status = status;
    if (body_off) *body_off = body;
    if (body_len) *body_len = length;
    return 1;
}

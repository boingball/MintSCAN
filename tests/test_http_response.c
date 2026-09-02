#include "http_response.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_interim_and_case_insensitive_length(void)
{
    static const char response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "content-length: 8\r\n"
        "Content-Type: application/ipp\r\n\r\n"
        "12345678";
    int first_body = ms_http_find_body(response, (int)sizeof(response) - 1, 0);
    int final_body;

    assert(first_body > 0);
    assert(ms_http_status(response, (int)sizeof(response) - 1, 0) == 100);
    final_body = ms_http_find_body(response, (int)sizeof(response) - 1, first_body);
    assert(final_body > first_body);
    assert(ms_http_status(response, (int)sizeof(response) - 1, first_body) == 200);
    assert(ms_http_content_length(response, first_body, final_body) == 8);
}

static void test_chunked_body(void)
{
    static const char expected[] = { 1, 1, 0, 0, 0, 0, 0, 1 };
    char body[] =
        "4\r\n\x01\x01\x00\x00\r\n"
        "4;printer=yes\r\n\x00\x00\x00\x01\r\n"
        "0\r\nX-Test: yes\r\n\r\n";
    int encoded_len = (int)sizeof(body) - 1;
    int decoded_len;
    int prefix_len;

    for (prefix_len = 0; prefix_len < encoded_len; ++prefix_len)
        assert(ms_http_chunked_complete(body, prefix_len) == 0);
    assert(ms_http_chunked_complete(body, encoded_len) == 1);
    decoded_len = ms_http_decode_chunked(body, encoded_len);
    assert(decoded_len == (int)sizeof(expected));
    assert(memcmp(body, expected, sizeof(expected)) == 0);
}

static void test_transfer_encoding_token(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip, Chunked\r\n\r\n";
    int body = ms_http_find_body(response, (int)sizeof(response) - 1, 0);

    assert(body > 0);
    assert(ms_http_header_has_token(response, 0, body,
                                    "Transfer-Encoding", "chunked") == 1);
}

static void test_final_chunked_ipp_response(void)
{
    static const char expected[] = { 1, 1, 0, 0, 0, 0, 0, 1 };
    char response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "8\r\n"
        "\x01\x01\x00\x00\x00\x00\x00\x01\r\n"
        "0\r\n\r\n";
    int response_len = (int)sizeof(response) - 1;
    int http_status = 0;
    int body_off = -1;
    int body_len = -1;

    assert(ms_http_final_body(response, response_len - 1, &http_status,
                              &body_off, &body_len) == 0);
    assert(ms_http_final_body(response, response_len, &http_status,
                              &body_off, &body_len) == 1);
    assert(http_status == 200);
    assert(body_len == (int)sizeof(expected));
    assert(memcmp(response + body_off, expected, sizeof(expected)) == 0);
}

static void test_mixed_case_header_copy(void)
{
    char value[64];
    const char response[] =
        "HTTP/1.1 201 Created\r\n"
        "lOcAtIoN: /eSCL/ScanJobs/42\r\n"
        "Content-Length: 0\r\n\r\n";
    int body = ms_http_find_body(response, (int)strlen(response), 0);
    assert(body > 0);
    assert(ms_http_copy_header(response, 0, body, "Location",
                               value, sizeof(value)));
    assert(strcmp(value, "/eSCL/ScanJobs/42") == 0);
}

int main(void)
{
    test_interim_and_case_insensitive_length();
    test_chunked_body();
    test_transfer_encoding_token();
    test_final_chunked_ipp_response();
    test_mixed_case_header_copy();
    puts("HTTP response parser tests passed");
    return 0;
}

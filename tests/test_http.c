#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "server.h"
#include "http.h"

static int passed = 0;
static int failed = 0;

static void check(int condition, const char *test_name) {
    if (condition) passed++;
    else {
        failed++;
        printf(" [FAIL] %s\n", test_name);
    }
}

static int str_eq(const char *a, const char *b) {
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static void set_query(HttpRequest *req, const char *q) {
    memset(req, 0, sizeof(*req));
    strncpy(req->query, q, sizeof(req->query) - 1);
}

static int read_response(int fd, char *buf, size_t buf_size) {
    struct timeval tv = {0, 50000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t total = 0;
    while (total < buf_size - 1) {
        ssize_t n = read(fd, buf + total, buf_size - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (int)total;
}

static void test_decode_component(void) {
    char s[64];

    strcpy(s, "Igor");
    check(decode_component(s, DECODE_QUERY) == 0 && str_eq(s, "Igor"),
          "decode_component: no changes");

    strcpy(s, "Igor%20Suvic");
    check(decode_component(s, DECODE_QUERY) == 0 && str_eq(s, "Igor Suvic"),
          "decode_component: %20 -> whitespace");

    strcpy(s, "a+b");
    check(decode_component(s, DECODE_QUERY) == 0 && str_eq(s, "a b"),
          "decode_component: + -> whitespace");

    strcpy(s, "%41%42");
    check(decode_component(s, DECODE_QUERY) == 0 && str_eq(s, "AB"),
          "decode_component: %41%42 -> AB");

    strcpy(s, "100%");
    check(decode_component(s, DECODE_QUERY) == 0 && str_eq(s, "100%"),
          "decode_component: unfinished % is copied");

    strcpy(s, "%ZZ");
    check(decode_component(s, DECODE_QUERY) == -1,
          "decode_component: bad hex -> -1");

    strcpy(s, "a%00b");
    check(decode_component(s, DECODE_QUERY) == -1,
          "decode_component: NUL rejected");
}

static void test_decode_path(void) {
    char s[64];

    // the whole point of 6.5: a percent-encoded path reaches its route
    strcpy(s, "/hel%6co");
    check(decode_component(s, DECODE_PATH) == 0 && str_eq(s, "/hello"),
          "path: %6c -> l");

    strcpy(s, "/%68ello");
    check(decode_component(s, DECODE_PATH) == 0 && str_eq(s, "/hello"),
          "path: %68 -> h");

    strcpy(s, "/%41%42");
    check(decode_component(s, DECODE_PATH) == 0 && str_eq(s, "/AB"),
          "path: hex decode");

    strcpy(s, "/a%2Fb");
    check(decode_component(s, DECODE_PATH) == 0 && str_eq(s, "/a/b"),
          "path: %2F -> /");

    // same input, different mode, different result. These two checks are the
    // only ones that prove DecodeMode is doing anything at all.
    strcpy(s, "a+b");
    check(decode_component(s, DECODE_PATH) == 0 && str_eq(s, "a+b"),
          "path: '+' stays '+'");
    strcpy(s, "a+b");
    check(decode_component(s, DECODE_QUERY) == 0 && str_eq(s, "a b"),
          "query: '+' -> whitespace");

    strcpy(s, "a%ZZb");
    check(decode_component(s, DECODE_PATH) == -1,
          "path: bad hex -> -1");

    strcpy(s, "a%00b");
    check(decode_component(s, DECODE_PATH) == -1,
          "path: NUL rejected");
}

static void test_get_query_param(void) {
    HttpRequest req;
    char value[64];

    set_query(&req, "name=Igor");
    check(get_query_param(&req, "name", value, sizeof(value)) == 0 && str_eq(value, "Igor"),
        "get_query_param: found name=Igor");

    set_query(&req, "a=1&name=Igor");
    check(get_query_param(&req, "name", value, sizeof(value)) == 0 && str_eq(value, "Igor"),
        "get_query_param: second param");

    set_query(&req, "name=Igor%20Srce");
    check(get_query_param(&req, "name", value, sizeof(value)) == 0 && str_eq(value, "Igor Srce"),
        "get_query_param: url decoded value");

    set_query(&req, "name=a=b");
    check(get_query_param(&req, "name", value, sizeof(value)) == 0 && str_eq(value, "a=b"),
        "get_query_param: value contains =");

    set_query(&req, "a=1&b=2");
    check(get_query_param(&req, "name", value, sizeof(value)) == 1,
        "get_query_param: missing -> 1");

    set_query(&req, "name=%ZZ");
    check(get_query_param(&req, "name", value, sizeof(value)) == -1,
        "get_query_param: bad percent encoding -> -1");
}

static void test_parse_headers(void) {
    HttpRequest req;
    memset(&req, 0, sizeof(req));
    char buf[128];
    strcpy(buf, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    parse_request_line(buf, &req);
    parse_headers(buf, &req); 
    check(req.header_count == 1, "parse_headers: 1 header");
    check(str_eq(req.headers[0].name, "Host"), "parse_headers: header name");
    check(str_eq(req.headers[0].value, "x"), "parse_headers: header value");

    // multiple headers
    char buf2[128];
    strcpy(buf2, "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: curl\r\n\r\n");
    memset(&req, 0, sizeof(req));
    parse_request_line(buf2, &req);
    parse_headers(buf2, &req);
    check(req.header_count == 2, "parse_headers: 2 headers");

    // no colon -> -1
    char buf3[128];
    strcpy(buf3, "GET / HTTP/1.1\r\nWeirdNoColon\r\n\r\n");
    memset(&req, 0, sizeof(req));
    parse_request_line(buf3, &req);
    check(parse_headers(buf3, &req) == -1, "parse_headers: no colon -> -1");

    // 33 headers -> -1
    char buf4[2048];
    int n = snprintf(buf4, sizeof(buf4), "GET / HTTP/1.1\r\n");
    for (int i = 0; i < 33; i++) n += snprintf(buf4 + n, sizeof(buf4) - n, "X-H%d: v\r\n", i);
    memset(&req, 0, sizeof(req));
    parse_request_line(buf4, &req);
    check(parse_headers(buf4, &req) == -1, "parse_headers: 33 headers -> -1");
}

static void test_parse_request_line(void) {
    HttpRequest req;
    memset(&req, 0, sizeof(req));
    char buf[128], backup[128];
    strcpy(buf, "GET /hello?name=Igor HTTP/1.1\r\nHost: x\r\n\r\n");
    memcpy(backup, buf, sizeof(buf));
    parse_request_line(buf, &req);
    check(memcmp(buf, backup, sizeof(buf)) == 0, "parse_request_line: buffer unchanged");
    
    // path and query split apart
    strcpy(buf, "GET /hello?name=Igor HTTP/1.1\r\n");
    memset(&req, 0, sizeof(req));
    check(parse_request_line(buf, &req) == 0, "parse_request_line: success");
    check(str_eq(req.method, "GET"), "parse_request_line: method");
    check(str_eq(req.path, "/hello"), "parse_request_line: path without query");
    check(str_eq(req.query, "name=Igor"), "parse_request_line: query");
    check(str_eq(req.version, "HTTP/1.1"), "parse_request_line: version");

    // no query string
    strcpy(buf, "GET / HTTP/1.1\r\n");
    memset(&req, 0, sizeof(req));
    parse_request_line(buf, &req);
    check(str_eq(req.query, ""), "parse_request_line: empty query");

    // multiple params
    strcpy(buf, "GET /a?b=1&c=2 HTTP/1.1\r\n");
    memset(&req, 0, sizeof(req));
    parse_request_line(buf, &req);
    check(str_eq(req.query, "b=1&c=2"), "parse_request_line: multiple params");

    // too few tokens -> -1
    strcpy(buf, "GET\r\n");
    memset(&req, 0, sizeof(req));
    check(parse_request_line(buf, &req) == -1, "parse_request_line: too few tokens -> -1");
}

static void test_validate_request(void) {
    HttpRequest req;
    memset(&req, 0, sizeof(req));

    strcpy(req.method, "GET");
    strcpy(req.version, "HTTP/1.1");
    strcpy(req.path, "/");
    check(validate_request(&req) == 0, "validate_request: GET / -> 0");

    strcpy(req.path, "/hello");
    check(validate_request(&req) == 0, "validate_request: GET /hello -> 0");

    strcpy(req.method, "POST");
    check(validate_request(&req) == 1, "validate_request: POST -> 1 (405)");

    strcpy(req.method, "GET");
    strcpy(req.version, "HTTP/1.0");
    check(validate_request(&req) == -1, "validate_request: HTTP/1.0 -> -1 (400)");

    strcpy(req.version, "HTTP/1.1");
    strcpy(req.path, "hello");
    check(validate_request(&req) == -1, "validate_request: path without / -> -1 (400)");
}

static void test_send_response(void) {
    int fds[2];
    char response[8192];

    // 1. small body
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    char body[] = "Hello!";
    check(send_response(fds[0], 200, "OK", body, "", WITH_BODY) == 0, "send_response: small body returns 0");
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "HTTP/1.1 200 OK") != NULL, "send_response: status line");
    check(strstr(response, "Content-Length: 6") != NULL, "send_response: Content-Length matches body");
    check(strstr(response, "Hello!") != NULL, "send_response: body present");
    check(strstr(response, "Date: ") != NULL, "send_response: has Date header");
    check(strstr(response, "GMT\r\n") != NULL, "send_response: Date ends with GMT");
    close(fds[0]);
    close(fds[1]);

    // 2. large body - THIS ONE FAILS BEFORE THE FIX
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    char big_body[5001];
    memset(big_body, 'x', 5000);
    big_body[5000] = '\0';

    check(send_response(fds[0], 200, "OK", big_body, "", WITH_BODY) == 0, "send_response: 5000 byte body returns 0");
    int total = read_response(fds[1], response, sizeof(response));
    check(strstr(response, "Content-Length: 5000") != NULL, "send_response: large Content-Length");
    check(total > 5000, "send_response: full large body received");

    // verify the body is intact and not truncated
    char *body_start = strstr(response, "\r\n\r\n");
    check(body_start != NULL && (int)strlen(body_start + 4) == 5000, "send_response: body not truncated");
    close(fds[0]);
    close(fds[1]);

    // 3. error status
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    send_response(fds[0], 404, "Not Found", "Not Found\n", "", WITH_BODY);
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "HTTP/1.1 404 Not Found") != NULL, "send_response: 404 status line");
    close(fds[0]);
    close(fds[1]);

    // 4. empty body - covers the early return branch
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    check(send_response(fds[0], 200, "OK", "", "", WITH_BODY) == 0, "send_response: empty body returns 0");
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "Content-Length: 0") != NULL, "send_response: empty Content-Length");
    close(fds[0]);
    close(fds[1]);
}

static void test_get_header(void) {
    HttpRequest req;
    char value[64];
    char buf[128];

    // exact match
    strcpy(buf, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    memset(&req, 0, sizeof(req));
    parse_request_line(buf, &req);
    parse_headers(buf, &req);
    check(get_header(&req, "Host", value, sizeof(value)) == 0, "get_header: found Host");
    check(str_eq(value, "example.com"), "get_header: Host value");

    // case-insensitive: all variants find the same header
    check(get_header(&req, "host", value, sizeof(value)) == 0, "get_header: lowercase query name");
    check(get_header(&req, "HOST", value, sizeof(value)) == 0, "get_header: uppercase query name");
    check(get_header(&req, "HoSt", value, sizeof(value)) == 0, "get_header: mixed case query name");

    // non-existent -> 1
    check(get_header(&req, "X-Nope", value, sizeof(value)) == 1, "get_header: missing -> 1");
}

static void test_handle_client_requires_host(void) {
    int fds[2];
    char response[2048];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    const char *req = "GET / HTTP/1.1\r\n\r\n";
    send(fds[1], req, strlen(req), 0);

    set_client_timeout(fds[0], 1);
    handle_client(fds[0]);

    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "400 Bad Request") != NULL, "handle_client: missing Host -> 400");
    close(fds[0]);
    close(fds[1]);
}

static void test_send_response_without_body(void) {
    int fds[2];
    char response[4096];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    const char *body = "This body must never be sent.\n";

    check(send_response(fds[0], 200, "OK", body, "", WITHOUT_BODY) == 0,
        "send_response: WITHOUT_BODY returns 0");

    read_response(fds[1], response, sizeof(response));

    // Content-Length MUST describe the body we deliberately did not send
    const char *cl_prefix = "Content-Length: ";
    char *len_line = strstr(response, cl_prefix);
    int declared = len_line ? atoi(len_line + strlen(cl_prefix)) : -1;
    check(declared == (int)strlen(body), "send_response: WITHOUT_BODY keeps real Content-Length");

    // nothing at all after the header terminator
    char *body_start = strstr(response, "\r\n\r\n");
    check(body_start != NULL && strlen(body_start + 4) == 0,
        "send_response: WITHOUT_BODY sends zero body bytes");

    // and the body text is provably absent
    check(strstr(response, "never be sent") == NULL,
        "send_response: body text absent from response");

    close(fds[0]);
    close(fds[1]);
}

static void test_head_response(void) {
    int fds[2];
    char get_resp[4096];
    char head_resp[4096];

    // GET /about
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    set_client_timeout(fds[0], 1);
    const char *get_req = "GET /about HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], get_req, strlen(get_req), 0);
    handle_client(fds[0]);
    read_response(fds[1], get_resp, sizeof(get_resp));
    close(fds[0]);
    close(fds[1]);

    // HEAD /about
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    set_client_timeout(fds[0], 1);
    const char *head_req = "HEAD /about HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], head_req, strlen(head_req), 0);
    handle_client(fds[0]);
    read_response(fds[1], head_resp, sizeof(head_resp));
    close(fds[0]);
    close(fds[1]);

    check(strstr(head_resp, "HTTP/1.1 200 OK") != NULL, "head: 200 OK");

    // Content-Length MUST be identical to GET
    const char *cl_prefix = "Content-Length: ";
    char *get_len = strstr(get_resp, cl_prefix);
    char *head_len = strstr(head_resp, cl_prefix);
    check(get_len != NULL && head_len != NULL, "head: both have Content-Length");
    check(get_len != NULL && head_len != NULL &&
          atoi(get_len + strlen(cl_prefix)) == atoi(head_len + strlen(cl_prefix)),
        "head: Content-Length identical to GET");

    // but ZERO bytes of body
    char *body_start = strstr(head_resp, "\r\n\r\n");
    check(body_start != NULL && strlen(body_start + 4) == 0, "head: zero bytes of body");
}

static void test_rejects_other_methods(void) {
    int fds[2];
    char response[4096];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    set_client_timeout(fds[0], 1);
    const char *req = "DELETE / HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], req, strlen(req), 0);
    handle_client(fds[0]);

    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "405 Method Not Allowed") != NULL, "validate: DELETE still 405");
    check(strstr(response, "Allow: GET, HEAD") != NULL, "validate: Allow lists HEAD");
    close(fds[0]);
    close(fds[1]);
}

static void test_head_error_path_has_no_body(void) {
    int fds[2];
    char response[4096];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    set_client_timeout(fds[0], 1);
    const char *req = "HEAD / HTTP/1.1\r\n\r\n";
    send(fds[1], req, strlen(req), 0);
    handle_client(fds[0]);

    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "400 Bad Request") != NULL, "head: missing Host -> 400");
    char *body_start = strstr(response, "\r\n\r\n");
    check(body_start != NULL && strlen(body_start + 4) == 0,
        "head: error response has no body");
    close(fds[0]);
    close(fds[1]);
}

static void test_decoded_path_routes(void) {
    int fds[2];
    char response[4096];

    // Every other test in this file covers one function. This one covers the
    // pipeline parse -> validate -> decode -> route, which is where the
    // percent-decoding bug lived: each stage was individually correct.
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    set_client_timeout(fds[0], 1);
    const char *encoded = "GET /hel%6co HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], encoded, strlen(encoded), 0);
    handle_client(fds[0]);
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "HTTP/1.1 200 OK") != NULL,
          "pipeline: percent-encoded path reaches its route");
    check(strstr(response, "Hello!") != NULL,
          "pipeline: percent-encoded path returns the route body");
    close(fds[0]);
    close(fds[1]);

    // a malformed path is a bad request, not a missing resource
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    set_client_timeout(fds[0], 1);
    const char *bad = "GET /hello%ZZ HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], bad, strlen(bad), 0);
    handle_client(fds[0]);
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "400 Bad Request") != NULL,
          "pipeline: bad percent-encoding in path -> 400");
    close(fds[0]);
    close(fds[1]);

    // decoding must happen exactly once: %2520 is a literal %20, not a space
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    set_client_timeout(fds[0], 1);
    const char *once = "GET /hello%2520 HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], once, strlen(once), 0);
    handle_client(fds[0]);
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "HTTP/1.1 404 Not Found") != NULL,
          "pipeline: decoded once, not twice");
    close(fds[0]);
    close(fds[1]);

    // %00 must not truncate "/hello%00" into a different, matching route
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    set_client_timeout(fds[0], 1);
    const char *nul = "GET /hello%00 HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fds[1], nul, strlen(nul), 0);
    handle_client(fds[0]);
    read_response(fds[1], response, sizeof(response));
    check(strstr(response, "400 Bad Request") != NULL,
          "pipeline: NUL in path -> 400, not a truncated route");
    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    test_parse_headers();
    test_parse_request_line();
    test_validate_request();
    test_decode_component();
    test_decode_path();
    test_get_query_param();
    test_send_response();
    test_send_response_without_body();
    test_get_header();
    test_handle_client_requires_host();
    test_head_response();
    test_head_error_path_has_no_body();
    test_rejects_other_methods();
    test_decoded_path_routes();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
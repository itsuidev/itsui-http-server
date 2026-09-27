#include <stdio.h>
#include <string.h>

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

static void test_url_decode(void) {
    char s[64];

    strcpy(s, "Igor");
    check(url_decode(s) == 0 && str_eq(s, "Igor"), "url_decode: no changes");

    strcpy(s, "Igor%20Suvic");
    check(url_decode(s) == 0 && str_eq(s, "Igor Suvic"), "url_decode: %20 -> whitespace");

    strcpy(s, "a+b");
    check(url_decode(s) == 0 && str_eq(s, "a b"), "url_decode: + -> whitespace");

    strcpy(s, "%41%42");
    check(url_decode(s) == 0 && str_eq(s, "AB"), "url_decode: %41%42 -> AB");

    strcpy(s, "100%");
    check(url_decode(s) == 0 && str_eq(s, "100%"), "url_decode: unfinished % is copied");

    strcpy(s, "%ZZ");
    check(url_decode(s) == -1, "url_decode: bad hex -> -1");
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

int main(void) {
    test_parse_headers();
    test_parse_request_line();
    test_validate_request();
    test_url_decode();
    test_get_query_param();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
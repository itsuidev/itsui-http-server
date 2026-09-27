# itsui-http-server

A minimal HTTP/1.1 server written in C, built from scratch on raw BSD sockets.
No frameworks, no dependencies — just `socket`, `bind`, `accept` and the standard
library.

The goal is understanding how an HTTP server actually works at the syscall
level: request parsing, connection lifecycle, concurrency, and failure handling.

## What it does

- Parses the request line, headers and query string
- Validates method, version, path and the mandatory `Host` header
- Looks up headers case-insensitively (`host`, `HOST`, `HoSt` all match)
- Decodes percent-encoding in both the path and query values, exactly once
- Answers `GET` and `HEAD`; `HEAD` returns identical headers with no body
- Sends a `Date` header in every response, in UTC
- Handles one connection per forked child process
- Enforces read/write timeouts so slow clients cannot block the server
- Ignores `SIGPIPE` and `SIGCHLD` so client disconnects and finished children
  cannot take the process down
- 91 unit tests, verified by mutation testing

### Percent-decoding

The path and the query string are both percent-decoded, but `+` means different
things in each, so one decoder takes an explicit `DecodeMode`:

| | `+` | `%2F` | `%00` |
|---|---|---|---|
| Path (`DECODE_PATH`) | literal `+`, RFC 3986 §3.3 | `/` | `400` |
| Query value (`DECODE_QUERY`) | a space, `x-www-form-urlencoded` | `/` | `400` |

A query string is `application/x-www-form-urlencoded`, where `+` really does mean
a space. A path is not: `+` is a valid path character, so a file named `a+b.html`
has to stay reachable at `/a+b.html`. One `int` flag would have hidden that
difference at the call site, which is the same reason `ResponseBody` is an enum
rather than an `int`.

`%00` is rejected rather than decoded. Decoded it is a NUL, which terminates the
string, so `/hello%00` would be seen by the router as `/hello` and match a route
the client never asked for. Harmless while every route is a hard-coded string;
the moment a path reaches the filesystem it is a NUL-byte injection.

Decoding happens once, in `handle_client`, after `validate_request` and before
routing. Once, because a second pass would turn `/%2520` into a space. In that
position because the pipeline it belongs to is

```
raw bytes -> syntax check -> decode -> path traversal check -> filesystem
```

and the traversal check has to see decoded bytes: `..%2f` and `%2e%2e%2f` are the
same path once decoded.


## Requirements

- Linux (or WSL2)
- GCC
- GNU Make

Built and tested with GCC 13 on WSL2 / Ubuntu.

## Build and run

```bash
make          # builds ./server
./server      # listens on port 8000
```

```bash
make test     # runs the unit tests
make clean    # removes objects, binary and test binary
```

The build is warning-clean under `-Wall -Wextra`, and `make test` depends on
`all` so the server binary can never be older than the tests that pass.

## Routes

| Method | Path | Query | Response |
|---|---|---|---|
| `GET` | `/` | — | `Welcome to my C HTTP server!` |
| `GET` | `/about` | — | `This is my custom C HTTP server.` |
| `GET` | `/hello` | — | `Hello!` |
| `GET` | `/hello` | `name=Igor` | `Hello, Igor!` |
| `GET` or `HEAD` | anything else | — | `404 Not Found` |

```bash
curl http://localhost:8000/
curl "http://localhost:8000/hello?name=Igor%20Srce"   # -> Hello, Igor Srce!
curl -I http://localhost:8000/about                   # -> headers only, same Content-Length
```

## Response headers

| Header | Value | Source |
|---|---|---|
| `Content-Type` | `text/plain; charset=utf-8` | `send_response` |
| `Content-Length` | length of the body that *would* be sent | `send_response` |
| `Date` | IMF-fixdate, UTC | `send_response` |
| `Connection` | `close` | `send_response` |
| `Allow` | `GET, HEAD` | only on `405` |

### `405` versus `501`

These two answers look similar and mean opposite things, so the distinction is
made deliberately rather than by accident:

- `405 Method Not Allowed` — the method is a real HTTP method, and this resource
  does not accept it. `Allow` is **required** (RFC 9110 §15.5.6).
- `501 Not Implemented` — the token is not a method at all. `Allow` is
  **meaningless**, because listing methods would contradict the message: it would
  read as "use one of these instead", which is exactly the `405` case.

So `POST` is a `405` and `BREW` is a `501`:

```
$ printf 'POST / HTTP/1.1\r\nHost: x\r\n\r\n'      | nc 127.0.0.1 8000 | head -1
HTTP/1.1 405 Method Not Allowed
$ printf 'BREW / HTTP/1.1\r\nHost: x\r\n\r\n'      | nc 127.0.0.1 8000 | head -1
HTTP/1.1 501 Not Implemented
```

The boundary is the `KNOWN_METHODS` table in `src/http.c`, and it answers a
narrower question than "what do we support":

> Is this token a method name defined by the HTTP specifications?

Support is already communicated through `Allow`, so it is not the table's job.
The eight core methods are RFC 9110; `PATCH` is RFC 5789 rather than RFC 9110,
but it is deployed by essentially every REST API, so calling it undefined would
be wrong. Adding a method is one line in the array and nothing else.

Method names are **case-sensitive** (RFC 9110 §9.1), which falls out of using
`strcmp`: `get` is a `501`, not a misspelled `GET`.

`Date` and `Content-Length` are built inside `send_response` rather than at the
call sites, so a response cannot go out without them. `Content-Length` is
computed before the body is optionally skipped, which is what makes a `HEAD`
response report the length of the body it deliberately did not send.

## Rejected requests

Every row was reproduced by hand against a running server, and every threshold
below is the exact byte at which the behaviour flips.

| Condition | Status | Boundary |
|---|---|---|
| Known method other than `GET` or `HEAD` | `405 Method Not Allowed` + `Allow: GET, HEAD` | — |
| Unrecognised method token | `501 Not Implemented`, no `Allow` | — |
| Version other than `HTTP/1.1` | `400 Bad Request` | — |
| Path not starting with `/` | `400 Bad Request` | — |
| Missing `Host` header | `400 Bad Request` | — |
| Request line with fewer than 3 tokens | `400 Bad Request` | — |
| Header without a `:` | `400 Bad Request` | — |
| Request head larger than 1 KB | `400 Bad Request` | 918 B accepted, 1027 B rejected |
| Request target longer than 255 bytes | `400 Bad Request` | 255 B accepted, 256 B rejected |
| More than 32 headers | `400 Bad Request` | 32 accepted, 33 rejected |
| Header name of 64 bytes or more | `400 Bad Request` | 63 B accepted, 64 B rejected |
| Header value of 256 bytes or more | `400 Bad Request` | 255 B accepted, 256 B rejected |
| Invalid percent-encoding in path or query value | `400 Bad Request` | `%ZZ` rejected, `%20` accepted |
| `%00` anywhere in path or query value | `400 Bad Request` | see *Percent-decoding* above |

An unfinished `%` is deliberately *not* an error: `%ZZ` is a bad hex pair and is
rejected, while a trailing `100%` is copied through unchanged. That asymmetry is
covered by a test.

The head boundary is not exactly 1 KB because `read_request` stops at
`BUFFER_SIZE - 1` = 1023 bytes, and a request that overshoots is cut off at a
byte boundary that depends on how the reads happened to land. The practical rule
is the one in the table: comfortably under 1 KB works, over it does not.

The request target boundary is reached by accident rather than by a check. The
request line is parsed with `sscanf("%15s %255s %15s")`, so a 256-byte target is
truncated to 255 and the leftover character is then read as the *version*, which
fails the version comparison. The request is rejected, but the reason the
rejection fires is not the one it looks like — see Limitations.

## Limits

All fixed at compile time in `include/common.h`.

| Constant | Value | Meaning |
|---|---|---|
| `PORT` | 8000 | listen port |
| `BACKLOG` | 5 | pending-connection queue handed to `listen()` |
| `BUFFER_SIZE` | 1024 | request head buffer, and response header buffer |
| `MAX_HEADERS` | 32 | parsed headers per request |
| `CLIENT_TIMEOUT_SEC` | 5 | `SO_RCVTIMEO` and `SO_SNDTIMEO` |

Field sizes live in `HttpRequest` in `include/http.h`: `method[16]`, `path[256]`,
`query[256]`, `version[16]`, and per header `name[64]`, `value[256]`.

## Project layout

```
include/
  common.h     constants, status codes, check_error macro
  http.h       HTTP types, ResponseBody enum, function declarations
  server.h     socket-level function declarations
src/
  main.c       server lifecycle: listen -> accept -> fork -> reap
  server.c     socket setup, timeouts, read/write helpers
  http.c       request parsing, validation, response building, routing
tests/
  test_http.c  unit tests
```

## Request lifecycle

```
start_server()                 socket -> SO_REUSEADDR -> bind -> listen
  |
  +-- accept_client()          block until a client connects
  +-- set_client_timeout()     SO_RCVTIMEO + SO_SNDTIMEO, 5s
  +-- fork()
        |
        +-- child:  close(server_fd)      do not hold the listening socket
        |          handle_client()
        |          close(client_fd)
        |          _exit(0)               do NOT flush stdio buffers
        |
        +-- parent: close(client_fd)      parent never touches the connection
                   loop back to accept

handle_client()
  read_request()      accumulate until "\r\n\r\n" or buffer full
  parse_request_line()  method, path, version -> split path from query
  validate_request()   classify method (known? allowed?), then version/path rules
  parse_headers()      fill headers[] (destroys the raw buffer via strtok)
  get_header()         case-insensitive header lookup
  handle_request()     route by path, send response
```

`body_mode` is derived from the request method *after* `parse_request_line`
succeeds. Above that point the method does not exist yet (request too large) or
is unusable (parse failed), so those two error paths always send a body rather
than reading uninitialised memory.

## Concurrency model

One process per connection, created with `fork()`. The parent immediately closes
its copy of the client socket and returns to `accept()`, so slow connections never
delay other clients.

Three details that this depends on:

- **`close(server_fd)` in the child.** `fork()` duplicates the listening socket.
  If the child keeps it open, the port stays bound even after the parent dies, and
  the server cannot be restarted.
- **`_exit(0)` in the child, never `exit()`.** `printf` is block-buffered when
  output is redirected. `exit()` would flush the child's *inherited copy of the
  parent's buffer*, duplicating every log line. `_exit()` skips flushing.
- **`signal(SIGCHLD, SIG_IGN)`.** Children are reaped automatically, so the
  process table does not fill with zombies. The trade-off is that `waitpid()`
  can no longer report *which* child exited — revisit this if per-connection exit
  codes are ever needed.

`SO_RCVTIMEO` and `SO_SNDTIMEO` bound both directions: a client that stops sending
mid-request, or that opens a connection and never reads the response, gets dropped
after 5 seconds instead of blocking the accept loop.

### A consequence of `_exit()`, measured

Because the child calls `_exit()`, buffered output is discarded by design. With
stdout redirected to a file (fully buffered, 4 KB), the buffer is only written
when it fills or when something calls `fflush` — and `SIGKILL` does neither.
Measured on a server logging every request, 30 requests, stdout to a file, then
`kill -9`:

```
before setvbuf():   0 bytes on disk
after  setvbuf():   the 3 startup messages survive (126 bytes)
```

`setvbuf(stdout, NULL, _IOLBF, 0)` as the first statement of `main()` makes the
startup messages survive. It does not make per-request logging safe: each child
inherits a copy of the buffer, and several children writing concurrently to a
shared `fd` interleave or overwrite each other. For per-request logging the
correct pattern is `snprintf` into a local buffer followed by a single
`write(2)` — one syscall, atomic, no shared stdio state across `fork()`, and
immune to `SIGKILL`.

## Testing

```bash
make test
# 91 passed, 0 failed
```

| Test function | Checks |
|---|---|
| `test_parse_request_line` | 9 |
| `test_parse_headers` | 6 |
| `test_validate_request` | 9 |
| `test_status_text` | 6 |
| `test_decode_component` | 7 |
| `test_decode_path` | 8 |
| `test_get_query_param` | 6 |
| `test_send_response` | 13 |
| `test_send_response_without_body` | 4 |
| `test_get_header` | 6 |
| `test_handle_client_requires_host` | 1 |
| `test_head_response` | 4 |
| `test_head_error_path_has_no_body` | 2 |
| `test_rejects_other_methods` | 2 |
| `test_unrecognised_method` | 3 |
| `test_decoded_path_routes` | 5 |

The pure-logic functions are tested directly. The functions that touch a socket
are tested through `socketpair(AF_UNIX, SOCK_STREAM)`: one end plays the client,
`handle_client` treats the other as a real connection, so the full parse →
validate → decode → route → respond path runs without a network or a port. Every
such test sets a socket timeout *before* calling the code under test, so a wiring
mistake fails in one second with a readable message instead of hanging forever.

`make test` exits non-zero on failure, so it can gate CI directly. No CI is
configured yet.

### Test strength

A passing suite only proves the code matches the tests. Mutation testing checks
the other direction: inject a deliberate fault, confirm a test notices.
Twenty-one faults were injected into the code; twenty produced a failing test, one
further attempt was rejected by the compiler before it could run, and **one is
still not covered** (below).

| Injected fault | Caught by |
|---|---|
| `validate_request` always returns `REQUEST_VALID` | `validate_request: POST -> REQUEST_METHOD_NOT_ALLOWED (405)` |
| `+` decoding disabled in `url_decode` | `url_decode: + -> whitespace` |
| multi-param query scanning broken | `get_query_param: second param` |
| percent-decode errors ignored | `get_query_param: bad percent encoding -> -1` |
| `=` inside a value parsed as last match | `get_query_param: value contains =` |
| `parse_request_line` writing to `buffer` | rejected at compile time (`const`) |
| `WITHOUT_BODY` ignored, body always sent | 3 tests, incl. `head: zero bytes of body` |
| `&&` turned into `\|\|` in `validate_request` | 6 tests, incl. `head: 200 OK` |
| `Allow` header reverted to `GET` only | `validate: Allow lists HEAD` |
| `Content-Length` computed from the sent body | 2 tests, incl. `head: Content-Length identical to GET` |
| `body_mode` forced to `WITH_BODY` in `handle_client` | `head: error response has no body` |
| `mode` check removed from the `+` branch | `path: '+' stays '+'` |
| `%00` check removed from `decode_component` | 3 tests, incl. `path: NUL rejected` |
| path decoding removed from `handle_client` | 4 tests, all in `pipeline: *` |
| `DECODE_PATH` swapped for `DECODE_QUERY` at the call site | **nothing — see below** |
| `is_known_method` check removed from `validate_request` | 5 tests, incl. `validate: FROBNICATE -> 501` |
| `strcmp` swapped for `strcasecmp` in `is_known_method` | 2 tests, incl. `validate: lowercase get -> 501` |
| `Allow` header added to the `501` response | `validate: 501 carries no Allow header` |
| `PATCH` removed from `KNOWN_METHODS` | `validate_request: PATCH -> REQUEST_METHOD_NOT_ALLOWED (405)` |
| `501` branch switched to answer `405` | 4 tests, incl. `validate: FROBNICATE -> 501` |
| `case HTTP_NOT_IMPLEMENTED` dropped from `status_text` | `status_text: 501` *and* a `-Wswitch` warning |

Two rows are worth reading twice.

The `body_mode` fault initially **passed all 64 tests**. The code was correct at
the time, but nothing protected it: forcing `body_mode` to `WITH_BODY` only
affects the error paths in `handle_client`, and no test sent a `HEAD` request down
one of them. `test_head_error_path_has_no_body` was added to close that gap, after
which the fault is caught. A correct function and a protected function are
different things, and only mutation testing tells them apart.

The `DECODE_PATH` → `DECODE_QUERY` fault **still passes all 91 tests**, re-verified
after this change, and that
is a real gap rather than a weak test. The two modes differ only in how they treat
`+`, and the difference is invisible unless a request's path contains a `+` that
changes whether it matches a route. No route contains one. `test_decode_path`
proves the modes behave differently, but it calls `decode_component` directly, so
it never sees which mode `handle_client` passes. Nothing currently pins that
call site. The mitigation is readability — `DECODE_PATH` in the source says "this
is a path" — but readability is not a test. Static file serving will close this
for free, since a file named `a+b` requested at `/a+b` and at `/a%2Bb` produces
different files and different responses.

### Tests cover functions, not pipelines

The percent-decoding bug is the clearest argument for this section. `url_decode`
and `handle_request` were each individually correct, and so were the tests around
them. The bug lived in the seam: nothing decoded the path between parsing and
routing, so no test that exercised one function at a time could ever have found
it. It took a hand-written request against a running server.

`test_decoded_path_routes` exists for that reason. It is the only test that sends
a request through `parse → validate → decode → route`, and removing the decoding
step fails four of its checks — more than any other mutation in the table.


## Performance

Measured with a threaded Python client on WSL2, 3 runs per configuration:

| Load | Throughput | Range | Failed |
|---|---|---|---|
| 200 sequential | ~465 req/s | 454–486 | 0 |
| 50 over 10 threads | ~417 req/s | 375–452 | 0 |
| 100 over 10 threads | ~441 req/s | 420–463 | 0 |
| 100 over 20 threads | ~410 req/s | 335–459 | 0 |
| 100 over 25 threads | ~439 req/s | 430–449 | 0 |

Process count stays at `1` and zombie count at `0` after all of it.

The more useful number is the shape of that table: **throughput is flat from 1
to 25 threads.** Concurrency buys nothing, because the cost is `fork()` and
`fork()` happens serially in the parent. This is the ceiling of the model, not a
tuning problem — `epoll` or a thread pool would remove it, and that is a
different program. The per-run spread (one burst measured 154–474 req/s) is
WSL2 scheduler noise; treat the table as "it holds up under load", not as a
benchmark. A single `fork()` per request is the obvious bottleneck by design.

### Status codes

`HttpStatus` is an enum, and `status_text()` maps it to a reason phrase. The
reason phrase is not a parameter, so a call site has nowhere to put a typo:

```c
send_response(client_fd, HTTP_NOT_FOUND, "Not Found\n", "", body_mode);
```

The phrase lives in exactly one place:

```c
const char *status_text(HttpStatus status) {
    switch (status) {
        case HTTP_OK:                   return "OK";
        case HTTP_BAD_REQUEST:          return "Bad Request";
        /* ... */
    }
    return "";   // unreachable; see below
}
```

The `switch` has no `default` case, and that is the point. Adding an enumerator
without a `case` is a `-Wswitch` warning, verified:

```
src/http.c:81:5: warning: enumeration value 'HTTP_NOT_IMPLEMENTED' not handled in switch [-Wswitch]
```

The trailing `return "";` is needed because GCC does not treat an exhaustive
switch as proof of a return — it emits `-Wreturn-type` without one. The two
warnings are complementary and both are wanted: `default: return "";` would
silence `-Wreturn-type` *and* kill the `-Wswitch` check, so the unreachable
return is the correct way to keep the enforcement.

This was not theoretical. Before the change, 13 of 14 call sites had a typo in
the reason phrase caught by a test; the 14th, `500` on the `fork()` failure path,
passed the whole suite at the time (78 tests) with the phrase misspelled as
`Internal Server Err`. There is no way to express that mistake now.

## Limitations

Known gaps, all verified by hand against the running server:

- **The path is decoded, but the query string is scanned before it is decoded.**
  `/hel%6co` routes correctly; so does `?name=Igor%20Srce`. There is no static
  file serving yet, so there is nothing to traverse to.
- **Multiple `Host` headers are accepted.** `get_header` returns the first match.
  RFC 9112 §3.2 requires rejecting the request with `400`.
- **An over-long request target returns `400`, not `414`.** RFC 9110 §15.5.15.
- **The absolute-form request target is rejected.** `GET http://host/path
  HTTP/1.1` must be accepted by an HTTP/1.1 server. RFC 9112 §3.2.2.
- **The request body is never read.** No `Content-Length` on requests, no
  `Transfer-Encoding: chunked`, no `Expect: 100-continue`.
- **No keep-alive.** Every response says `Connection: close` and the server does
  one request per connection, so a client fetching several resources pays a
  `fork()` and a TCP handshake for each.
- **No `Server` header.** Optional under RFC 9110 §10.2.3.
- **`BACKLOG` is 5.** A burst larger than the pending queue relies on kernel
  SYN retransmission rather than being accepted promptly.
- **Nothing pins which `DecodeMode` `handle_client` passes.** Swapping it for
  `DECODE_QUERY` passes all 91 tests, because the modes differ only in `+` and no
  route contains one. Recorded here rather than papered over with a contrived
  route; static file serving closes it.
- **An unfinished `%` is copied instead of rejected.** `100%` survives, while
  `%ZZ` is a `400`. Both are malformed in the same way and get different answers.
- **The request line is parsed with `sscanf`, which truncates instead of
  reporting.** `sscanf("%15s %255s %15s")` cannot fail on an over-long field — it
  returns 3 conversions and hands back a truncated value. Two consequences: a
  16-byte method is silently cut to 15 bytes and then reported as `501` (verified)
  instead of as a malformed request, and a 256-byte target leaves its last
  character to be read as the version. Both end in a `400`/`501`, so nothing is
  accepted that should not be, but the request is refused for the wrong reason
  and the diagnostic is misleading. `strtok` or an explicit length check per
  field would fail loudly instead.
- **No static file serving.** Every route is a hard-coded string comparison.

This is why the server is described as an HTTP/1.1 *subset*: it requires
HTTP/1.1 and gets `Host` and `Date` right, but the list above is what it does
not yet do.

## License

MIT

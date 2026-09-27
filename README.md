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
- Decodes percent-encoding in query values
- Answers `GET` and `HEAD`; `HEAD` returns identical headers with no body
- Sends a `Date` header in every response, in UTC
- Handles one connection per forked child process
- Enforces read/write timeouts so slow clients cannot block the server
- Ignores `SIGPIPE` and `SIGCHLD` so client disconnects and finished children
  cannot take the process down
- 64 unit tests, verified by mutation testing

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

`Date` and `Content-Length` are built inside `send_response` rather than at the
call sites, so a response cannot go out without them. `Content-Length` is
computed before the body is optionally skipped, which is what makes a `HEAD`
response report the length of the body it deliberately did not send.

## Rejected requests

Every row was reproduced by hand against a running server, and every threshold
below is the exact byte at which the behaviour flips.

| Condition | Status | Boundary |
|---|---|---|
| Method other than `GET` or `HEAD` | `405 Method Not Allowed` + `Allow: GET, HEAD` | — |
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
| Query value with invalid percent-encoding | `400 Bad Request` | `%ZZ` rejected, `%20` accepted |

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
  validate_request()   method/version/path rules
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
# 64 passed, 0 failed
```

| Test function | Checks |
|---|---|
| `test_parse_request_line` | 9 |
| `test_parse_headers` | 6 |
| `test_validate_request` | 5 |
| `test_url_decode` | 6 |
| `test_get_query_param` | 6 |
| `test_send_response` | 13 |
| `test_send_response_without_body` | 4 |
| `test_get_header` | 6 |
| `test_handle_client_requires_host` | 1 |
| `test_head_response` | 4 |
| `test_head_error_path_has_no_body` | 2 |
| `test_rejects_other_methods` | 2 |

The pure-logic functions are tested directly. The functions that touch a socket
are tested through `socketpair(AF_UNIX, SOCK_STREAM)`: one end plays the client,
`handle_client` treats the other as a real connection, so the full parse →
validate → route → respond path runs without a network or a port. Every such
test sets a socket timeout *before* calling the code under test, so a wiring
mistake fails in one second with a readable message instead of hanging forever.

`make test` exits non-zero on failure, so it can gate CI directly. No CI is
configured yet.

### Test strength

A passing suite only proves the code matches the tests. Mutation testing checks
the other direction: inject a deliberate fault, confirm a test notices. Ten
faults were injected into the code and every one produced a failing test; one
further attempt was rejected by the compiler before it could run.

| Injected fault | Caught by |
|---|---|
| `validate_request` always returns `0` | `validate_request: POST -> 1 (405)` |
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

The last row is the interesting one. It initially **passed all 64 tests**. The
code was correct at the time, but nothing protected it: forcing `body_mode` to
`WITH_BODY` only affects the error paths in `handle_client`, and no test sent a
`HEAD` request down one of them. `test_head_error_path_has_no_body` was added to
close that gap, after which the fault is caught. A correct function and a
protected function are different things, and only mutation testing tells them
apart.

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

## Limitations

Known gaps, all verified by hand against the running server:

- **The path is not percent-decoded before routing.** `/hel%6co` returns `404`
  even though it decodes to `/hello`. Query values *are* decoded, so the two
  halves of the request line behave differently. RFC 9110 §4.2.3.
- **Multiple `Host` headers are accepted.** `get_header` returns the first match.
  RFC 9112 §3.2 requires rejecting the request with `400`.
- **An unrecognised method returns `405`, not `501`.** `405` means "known method,
  not allowed here"; `501` means "this server does not implement the
  functionality". RFC 9110 §15.5.6.
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
- **Status codes are bare `#define`s.** Each one is spelled out as a string at
  every call site, with nothing keeping the two in sync.
- **The request line is parsed with `sscanf`, which truncates instead of
  reporting.** `sscanf("%15s %255s %15s")` cannot fail on an over-long field — it
  returns 3 conversions and hands back a truncated value. Two consequences: a
  16-byte method is silently cut to 15 bytes and then rejected as `405` instead
  of being reported as a malformed request, and a 256-byte target leaves its last
  character to be read as the version. Both end in a `400`/`405`, so nothing is
  accepted that should not be, but the request is refused for the wrong reason
  and the diagnostic is misleading. `strtok` or an explicit length check per
  field would fail loudly instead.
- **No static file serving.** Every route is a hard-coded string comparison.

This is why the server is described as an HTTP/1.1 *subset*: it requires
HTTP/1.1 and gets `Host` and `Date` right, but the list above is what it does
not yet do.

## License

MIT

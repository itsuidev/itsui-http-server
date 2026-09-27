# itsui-http-server

A minimal HTTP/1.1 server written in C, built from scratch on raw BSD sockets.
No frameworks, no dependencies — just `socket`, `bind`, `accept` and the standard
library.

The goal is understanding how an HTTP server actually works at the syscall
level: request parsing, connection lifecycle, concurrency, and failure handling.

## What it does

- Parses the request line, headers and query string
- Validates method, version and path
- Decodes percent-encoding in query values
- Handles one connection per forked child process
- Enforces read/write timeouts so slow clients cannot block the server
- Ignores `SIGPIPE` and `SIGCHLD` so client disconnects and finished children
  cannot take the process down
- 32 unit tests covering the entire parsing layer

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

The build is warning-clean under `-Wall -Wextra`.

## Routes

| Method | Path | Query | Response |
|---|---|---|---|
| `GET` | `/` | — | `Welcome to my C HTTP server!` |
| `GET` | `/hello` | `name=Igor` | `Hello, Igor!` |
| `GET` | `/hello` | — | `Hello!` |
| `GET` | `/about` | — | `This is my custom C HTTP server.` |
| `GET` | anything else | — | `404 Not Found` |

```bash
curl http://localhost:8000/
curl "http://localhost:8000/hello?name=Igor%20Srce"   # -> Hello, Igor Srce!
```

## Rejected requests

| Condition | Status |
|---|---|
| Method other than `GET` | `405 Method Not Allowed` |
| Version other than `HTTP/1.1` | `400 Bad Request` |
| Path not starting with `/` | `400 Bad Request` |
| Request head larger than 1 KB | `400 Bad Request` |
| Header count above 32 | `400 Bad Request` |
| Header name above 63 bytes | `400 Bad Request` |
| Header value above 255 bytes | `400 Bad Request` |
| Request line with fewer than 3 tokens | `400 Bad Request` |

## Project layout

```
include/
  common.h     constants, status codes, check_error macro
  http.h       HTTP types and function declarations
  server.h     socket-level function declarations
src/
  main.c       server lifecycle: listen -> accept -> fork -> reap
  server.c     socket setup, timeouts, read/write helpers
  http.c       request parsing, validation, response building, routing
tests/
  test_http.c  unit tests for the parsing layer
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
  handle_request()     route by path, send response
```

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

## Testing

```bash
make test
# 32 passed, 0 failed
```

The tests cover every exported function in the parsing layer:
`parse_request_line`, `validate_request`, `parse_headers`, `url_decode` and
`get_query_param`. They are pure logic and need no sockets, so the suite runs in
well under a millisecond.

`make test` exits non-zero on failure, so it can gate CI directly.

Test strength was verified by mutation testing — six deliberate regressions were
introduced into the parser and each one produced a failing test:

| Injected fault | Caught by |
|---|---|
| `validate_request` always returns `0` | `validate_request: POST -> 1 (405)` |
| `+` decoding disabled in `url_decode` | `url_decode: + -> whitespace` |
| multi-param query scanning broken | `get_query_param: second param` |
| percent-decode errors ignored | `get_query_param: bad percent encoding -> -1` |
| `=` inside a value parsed as last match | `get_query_param: value contains =` |
| `parse_request_line` writing to `buffer` | rejected at compile time (`const`) |

## Performance

Measured with Python clients on WSL2, 32 parallel threads:

| Load | Result | Throughput |
|---|---|---|
| 200 sequential | 200/200 | ~136 req/s |
| 50 concurrent | 50/50 | ~409 req/s |
| 100 concurrent | 100/100 | ~425 req/s |

> These numbers are **not** representative. WSL2 adds significant syscall
> overhead, and the client is Python. Treat them as "it works under load", not as
> a benchmark. A single `fork()` per request is the obvious ceiling here.

Process count stays at `1` after 400 requests, and zombie count at `0`.

## License

MIT

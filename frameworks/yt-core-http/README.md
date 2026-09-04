# yt-core-http

The HTTP server from [`yt/yt/core/http`](https://github.com/ytsaurus/ytsaurus/tree/main/yt/yt/core/http), YTsaurus's own C++ networking library. This is the exact server every YTsaurus component (masters, proxies, schedulers, nodes) uses for its monitoring and API endpoints in production.

## Stack

- **Language:** C++20
- **Library:** `yt/yt/core/http` (part of the `ytsaurus` monorepo)
- **Build:** CMake + Conan, clang-20/lld-20, following [`BUILD.md`](https://github.com/ytsaurus/ytsaurus/blob/main/BUILD.md)

## Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/pipeline` | GET | Returns `ok` (plain text) |
| `/baseline11` | GET | Sums query parameter values |
| `/baseline11` | POST | Sums query parameters + request body |
| `/json/{count}?m=N` | GET | First `count` dataset items with `total = price * quantity * m`; compressed with gzip/br when `Accept-Encoding` asks for it. Served on both port 8080 (plaintext) and 8081 (TLS, HTTP/1.1 only) |
| `/echo` | POST | Returns the request body back verbatim, read through `ReadAll()` (Content-Length or chunked). Served on both port 8080 and 8081 |

The TLS listener on port 8081 only starts when `/certs/server.crt` and `/certs/server.key` are present (mounted only for TLS-subscribed profiles).

## Notes

- `NHttp::CreateServer` on `TServerConfig`, with one thread-pool poller thread per core -- the poller doubles as the handler invoker, matching how every production YTsaurus server sets this library up.
- Routing is `IServer::AddHandler(pattern, ...)`. Matching is copied from Go's old (pre-1.22) `ServeMux` semantics: exact match or trailing-slash prefix, no named path parameters -- there is nothing to extract `{count}` from, so an entry using this library for a parameterized route has to parse the path itself.
- The request body is drained with `IRequest::ReadAll()` (the library's own extension method on its async zero-copy input stream), which already handles both `Content-Length` and chunked `Transfer-Encoding` framing.
- The response is built procedurally -- `SetStatus`, then `GetHeaders()->Set(...)`, then `WriteBody(...)` -- rather than declared in one call.
- Query parameters are parsed by hand (`SumQueryParameters`/`FindQueryParameter` in `main.cpp`) since the library hands back only the raw query string (`IRequest::GetUrl().RawQuery`).
- `/json/{count}` has no named path parameter either -- `{count}` is parsed out of `IRequest::GetUrl().Path` by hand, the same way the query string is.
- JSON is built with `NYT::NJson::CreateJsonConsumer` (a YSON-consumer bridge, the same one `helpers.h`'s `ReplyJson` uses internally) driven through `NYT::NYTree::BuildYsonFluently`; the dataset itself is parsed once at startup with `library/cpp/json`'s own DOM reader, a separate library.
- Compression is `http/compression.h`'s `CreateCompressingAdapter`, which wraps the response writer directly (it is itself a flushable async output stream) -- gzip/br chosen per request via `GetBestAcceptedContentEncoding`, and `Content-Encoding` only set when the client actually asked for one.
- TLS is `NHttps::CreateServer` (`yt/yt/core/https`) on its own `TServerConfig` with `Credentials` pointing at the mounted cert/key. It shares the same `TRequestPathMatcher` as the plaintext server (`IServer::SetPathMatcher`) rather than registering every handler twice. `NCrypto::TSslContext` sets no ALPN callback at all, so the server never advertises one -- correct for an HTTP/1.1-only listener. Protocol range is hardcoded in the library to TLS 1.2-1.3 (`SSL_CTX_set_min/max_proto_version`), so a TLS 1.3 client gets 1.3.

## Completeness

Declared as `false` on all four axes in `meta.json`:

- **Routing** -- no path parameters, see above.
- **Middleware** -- no composable pipeline; the closest thing is `CreateErrorWrappingHttpHandler`, a single fixed wrapper, not an ordered chain.
- **Request** -- no query-parameter or header convenience beyond the raw strings; the body is a stream you drain yourself.
- **Response** -- built by mutating `IResponseWriter` across separate calls, not declared in one call.

Worth a second look now that `/json` exists: `ReplyJson` (and the `BuildJsonBody`/`CreateJsonConsumer` path this entry actually uses) sets `Content-Type`, serializes and writes the body all from one function call, which reads closer to "declared" than the plain-text handlers' `SetStatus` + `GetHeaders()->Set` + `WriteBody` sequence. Left as `false` here since status still isn't part of that call and the compressed path has to fall out of it entirely to interpose `CreateCompressingAdapter` -- but this is exactly the kind of judgment call the docs expect a reviewer to weigh in on, not something to self-certify.

This entry exercises `baseline`, `pipelined`, `limited-conn`, `latency-1m`, `latency-10k`, `json-comp`, `json-tls` and `8gbit`.

# jsondaemon

A C daemon that loads every shared object of a directory as a plugin, asks it
where it wants to be mounted, and serves it as an HTTP/JSON (JSON-RPC 2.0) API.
See README.md for the user side.

## Layout

- `include/jsondaemon.h`: the plugin API, the only public header. Keep it
  backward compatible; bump `JD_API_VERSION` on any breaking change.
- `src/main.c`: options, signals, accept loop (one thread per connection).
- `src/http.c`: minimal HTTP/1.1 (Content-Length bodies, keep-alive,
  pipelining, Expect: 100-continue). No chunked request bodies.
- `src/registry.c`: plugin directory scanning and hot reload. Plugins are
  copied to a private shadow dir before `dlopen()`, so the originals can be
  overwritten in place. Only the main thread writes the registry.
- `src/rpc.c`: routing, JSON-RPC 2.0, plain `/<mount>/<method>` calls.
- `plugins/<name>/*.c`: example plugins, each built to `build/plugins/<name>.so`.
- `tests/`: end-to-end tests in Python (stdlib only) against the real binary;
  `tests/fixtures/` holds test-only plugins.

## Conventions

- C11 + POSIX, builds on Linux (gcc, clang) and macOS (clang). Tabs, 120 cols,
  kernel-like style, `/* */` comments explaining why, not what.
- Only dependency: json-c (via pkg-config).
- `make WERROR=1 test` must pass; on Linux `make SANITIZE=1 test` too.
- Every behavior change comes with an end-to-end test in `tests/`.
- Request threads hold the registry read lock for the whole plugin call; never
  take it twice in the same thread.

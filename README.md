# jsondaemon

[![CI](https://github.com/fclairamb/jsondaemon/actions/workflows/ci.yml/badge.svg)](https://github.com/fclairamb/jsondaemon/actions/workflows/ci.yml)

**Drop a `.so` in a directory: it is an HTTP/JSON API.**

jsondaemon watches a plugin directory. When a shared object shows up, it loads it, asks it which
HTTP path it wants to be mounted at, and serves it right away, as plain JSON calls and as
[JSON-RPC 2.0](https://www.jsonrpc.org/specification). Replace the file and the new version is live.
Remove it and the endpoint is gone. No restart in between.

A plugin is two C functions:

```c
#include <string.h>
#include <jsondaemon.h>

const char *jd_mount(void)
{
	return "/hello";
}

json_object *jd_call(const char *method, json_object *params, jd_error *err)
{
	if (strcmp(method, "greet") != 0)
		return jd_fail(err, JD_METHOD_NOT_FOUND, "unknown method '%s'", method);
	return json_object_new_string("Hello!");
}
```

```sh
$ cc -shared -fPIC $(pkg-config --cflags --libs json-c) hello.c -o plugins/hello.so
$ curl localhost:8080/hello/greet
"Hello!"
```

That makes it a cheap way to put an HTTP API in front of existing C code (a legacy library, a
computation kernel, a hardware driver) without rewriting it or adding a web framework to it.

## Why

The original jsondaemon was written by [Florent Clairambault](https://github.com/fclairamb) in the
early 2010s, on top of [json-c](https://github.com/json-c/json-c). Declaring two functions turned an
old-school C program into a JSON/HTTP RPC server, and it "worked like a charm". It was never open
sourced, and he regretted it.

This repository is a from-scratch reimplementation, written in 2026 by Claude (Anthropic's AI) from
a five-message chat description of the original.

## Quick start

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libjson-c-dev
# macOS
brew install json-c pkgconf

make
build/jsondaemon -d build/plugins
```

```sh
$ curl 'localhost:8080/hello/greet?name=Gui'
{"message":"Hello, Gui!"}

$ curl -d '[1, 2, 3.5]' localhost:8080/math/add
6.5

$ curl -d '{"jsonrpc":"2.0","method":"div","params":[1,0],"id":1}' localhost:8080/math
{"jsonrpc":"2.0","error":{"code":1001,"message":"division by zero"},"id":1}

$ curl localhost:8080/
{"name":"jsondaemon","version":"0.1.0","plugins":[{"mount":"/hello","file":"hello.so",...}]}
```

## Bundled plugins

| Plugin | Mount | Methods | Shows |
|---|---|---|---|
| [`hello`](plugins/hello/hello.c) | `/hello` | `greet` | the minimal plugin: just the two functions |
| [`math`](plugins/math/math.c) | `/math` | `add`, `sub`, `mul`, `div` | errors, `jd_describe()`, `JD_THREAD_SAFE` |
| [`sysinfo`](plugins/sysinfo/sysinfo.c) | `/sys` | `uname`, `loadavg`, `time` | wrapping plain libc calls, `jd_init()` / `jd_fini()` |

`math` takes its operands as an array (`[1, 2, 3]`) or as the values of an object (`{"a": 1, "b": 2}`).
Integers stay exact 64-bit integers as long as they can, then the computation switches to floating
point (`7 / 2` is `3.5`, `2^63 - 1 + 1` is `9.2233720368547758e+18`).

## HTTP API

| Request | Does |
|---|---|
| `GET /` | daemon version and every plugin, with its metadata and `jd_describe()` |
| `GET /_health` | `{"status":"ok","plugins":3}` |
| `GET /<mount>` | one plugin's metadata |
| `POST /<mount>` | JSON-RPC 2.0: a single request, a batch, notifications |
| `POST /<mount>/<method>` | plain call: the body is the params (or empty), the response is the result |
| `GET /<mount>/<method>?k=v` | plain call, params built from the query string |

On a plain call, a query string value that parses as JSON is used as such (`a=1` is the number 1,
`n=[1,2]` an array), anything else is a string (`name=Gui`). Plain calls answer `200` with the
result, or an error status with `{"error":{"code":...,"message":...}}`:

| Error code | HTTP status |
|---|---|
| `-32700` parse error, `-32600` invalid request, `-32602` invalid params | 400 |
| `-32601` method not found (or nothing mounted there) | 404 |
| `-32603` internal error, `-32000` to `-32099` server errors | 500 |
| anything else: an application error defined by the plugin | 422 |

The JSON-RPC endpoint always answers `200` with JSON-RPC responses (errors included), or `204` when
the request only held notifications.

Request bodies must be strict JSON (no `NaN`, no trailing commas, nothing after the value) and carry
a `Content-Length`: chunked request bodies are refused with `501`.

## Writing a plugin

Include [`jsondaemon.h`](include/jsondaemon.h) (`make install` puts it in `$PREFIX/include`) and
build a shared object that links json-c:

```sh
cc -shared -fPIC $(pkg-config --cflags json-c) -I/usr/local/include \
   my_plugin.c -o plugins/my_plugin.so $(pkg-config --libs json-c)
```

A plugin file must be named `*.so` (or `*.dylib`). Files starting with a dot are ignored, so a
deploy can write `.my_plugin.so.tmp` and rename it.

### Required exports

- **`const char *jd_mount(void)`**: the path to mount the plugin at, e.g. `"/hello"` or
  `"/billing/v2"`. It is asked first, right after loading. Letters, digits, `/`, `-`, `.`, `_` and
  `~` only. No trailing slash. Paths starting with `/_` are reserved for the daemon. The string is
  copied.
- **`json_object *jd_call(const char *method, json_object *params, jd_error *err)`**: handles a call.
  `params` is borrowed and may be `NULL`. Return a **new reference**: the daemon releases it.
  Returning `NULL` without an error means a `null` result. To fail, `return jd_fail(err, code,
  "printf-like %s", ...)`, and optionally set `err->data` (a new reference) for JSON-RPC
  `error.data`.

### Optional exports

- **`int jd_init(void)`**: runs once, after the mount has been accepted. Non-zero refuses the plugin.
- **`void jd_fini(void)`**: runs once, before the plugin is unloaded or replaced.
- **`json_object *jd_describe(void)`**: free-form self-description, shown under `about` by `GET /`
  and `GET /<mount>`.
- **`const unsigned jd_flags = JD_THREAD_SAFE;`**: see below.

`jd_param(params, index, "name")` fetches a parameter passed either by position or by name.

### Threads

Requests run on concurrent threads, but by default **the daemon never runs two calls of the same
plugin at the same time**: legacy code with static buffers and global state just works. A plugin
that is reentrant exports `jd_flags = JD_THREAD_SAFE` and its calls run in parallel.

json-c reference counts are not atomic: do not share a `json_object` between calls (return a fresh
one, or a deep copy of a cached one).

## Hot reload

The directory is rescanned every second (`-i`), and on `SIGHUP`.

- **New file**: loaded and mounted.
- **Changed file** (size, mtime or inode): the new version is loaded and initialized on the side,
  then swapped in atomically as soon as the calls in flight on the old version are done. The old
  version is unloaded right after.
- **Removed file**: unmounted, then unloaded once its calls in flight are done.
- **Broken file** (not a shared object, no `jd_mount`, invalid mount, `jd_init` failure): logged once
  and left alone until it changes. If it was an update, **the previous version keeps serving**.
- **Mount conflict**: the plugin already holding the mount (or the first file in alphabetical order)
  wins. The other one is mounted automatically as soon as the mount is free.

The daemon never `dlopen()`s your file directly: it copies it to a private temporary directory and
loads the copy. Overwriting a loaded plugin in place (`cp`, the linker writing its output) is
therefore safe. It would crash a process that mapped the original.

## Options

```
  -d, --plugins DIR          plugin directory (default: ./plugins)
  -l, --listen ADDR          address to bind (default: 127.0.0.1)
  -p, --port PORT            port, 0 for any free one (default: 8080)
  -i, --scan-interval MS     directory scan period, 0 to only scan at start
                             and on SIGHUP (default: 1000)
  -b, --max-body BYTES       largest accepted request body (default: 1048576)
  -c, --max-connections N    concurrent connections (default: 128)
  -v, --verbose              log every request
  -q, --quiet                only log warnings and errors
```

`SIGINT` / `SIGTERM` stop accepting connections, let the requests in progress finish, call every
`jd_fini()` and exit.

## Security

A plugin is native code running inside the daemon, with its privileges. A crash in a plugin takes
the daemon down, and anyone who can write to the plugin directory can run code. Keep that directory
as protected as the binary itself.

jsondaemon binds to `127.0.0.1` by default and has no TLS and no authentication: to expose it, put a
reverse proxy in front of it.

## Development

```sh
make WERROR=1 test          # build with -Werror, then the end-to-end tests
make SANITIZE=1 test        # same under AddressSanitizer + UBSan (Linux)
```

The tests (`tests/test_jsondaemon.py`, Python standard library only) start the real binary on a free
port and cover the HTTP layer (keep-alive, pipelining, `100-continue`, limits, malformed requests),
JSON-RPC, every bundled plugin, hot reload (add, replace, overwrite in place, remove, broken files,
mount conflicts, `SIGHUP`) and the per-plugin serialization of calls.

CI runs them on Linux (gcc and clang), on macOS, under sanitizers, and checks that `make install`
lets an out-of-tree plugin build against the installed header.

## License

[MIT](LICENSE)

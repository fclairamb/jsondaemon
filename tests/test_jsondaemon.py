"""End-to-end tests: start the real daemon, talk HTTP to it.

Run with `make test` (it sets JD_BIN, JD_PLUGINS and JD_FIXTURES).
"""

import http.client
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import unittest.mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("JD_BIN", os.path.join(ROOT, "build", "jsondaemon"))
PLUGINS = os.environ.get("JD_PLUGINS", os.path.join(ROOT, "build", "plugins"))
FIXTURES = os.environ.get("JD_FIXTURES", os.path.join(ROOT, "build", "fixtures"))


class Daemon:
    """A jsondaemon process on a free port, with its own plugin directory."""

    def __init__(self, *args):
        self.dir = tempfile.mkdtemp(prefix="jd-test-")
        self.lines = []
        self.cond = threading.Condition()
        self.proc = subprocess.Popen(
            [BIN, "-d", self.dir, "-p", "0", "-i", "50", "-v", *args],
            stderr=subprocess.PIPE,
            text=True,
        )
        self.reader = threading.Thread(target=self._read_logs, daemon=True)
        self.reader.start()
        line = self.wait_log(r"listening on http://127\.0\.0\.1:(\d+)")
        self.port = int(re.search(r":(\d+),", line).group(1))

    def _read_logs(self):
        for line in self.proc.stderr:
            with self.cond:
                self.lines.append(line.rstrip("\n"))
                self.cond.notify_all()

    def wait_log(self, pattern, timeout=5.0, start=0):
        """Waits for a log line matching `pattern`, returns it."""
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for line in self.lines[start:]:
                    if regex.search(line):
                        return line
                left = deadline - time.monotonic()
                if left <= 0 or self.proc.poll() is not None:
                    raise AssertionError(f"no log line matches {pattern!r}:\n" + "\n".join(self.lines))
                self.cond.wait(left)

    def mark(self):
        with self.cond:
            return len(self.lines)

    def add(self, src, name=None):
        """Drops a plugin in the directory the way a deploy would: write then rename."""
        name = name or os.path.basename(src)
        tmp = os.path.join(self.dir, "." + name + ".tmp")
        shutil.copyfile(src, tmp)
        os.rename(tmp, os.path.join(self.dir, name))

    def remove(self, name):
        os.unlink(os.path.join(self.dir, name))

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            if isinstance(body, (dict, list, int, float)) and not isinstance(body, bool):
                body = json.dumps(body)
            conn.request(method, path, body=body, headers=headers or {})
            res = conn.getresponse()
            raw = res.read()
            return res.status, json.loads(raw) if raw else None, res
        finally:
            conn.close()

    def get(self, path):
        return self.request("GET", path)

    def post(self, path, body=None):
        return self.request("POST", path, body)

    def wait_for(self, path, status, timeout=5.0):
        deadline = time.monotonic() + timeout
        while True:
            got = self.get(path)
            if got[0] == status or time.monotonic() > deadline:
                return got
            time.sleep(0.05)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.reader.join(timeout=5)
        self.proc.stderr.close()
        shutil.rmtree(self.dir, ignore_errors=True)
        return self.proc.returncode


def raw_exchange(port, payload, expect_responses=1, timeout=5.0):
    """Sends raw bytes, reads until `expect_responses` HTTP responses arrived."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(payload)
        data = b""
        deadline = time.monotonic() + timeout
        while data.count(b"HTTP/1.1 ") < expect_responses and time.monotonic() < deadline:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
        time.sleep(0.05)
        sock.setblocking(False)
        try:
            data += sock.recv(65536)
        except (BlockingIOError, OSError):
            pass
        return data


class ApiTest(unittest.TestCase):
    """The shipped plugins and the HTTP surface."""

    @classmethod
    def setUpClass(cls):
        cls.d = Daemon("-b", "65536")
        for name in ("hello.so", "math.so", "sysinfo.so"):
            cls.d.add(os.path.join(PLUGINS, name))
        cls.d.wait_for("/sys", 200)
        cls.d.wait_for("/math", 200)
        cls.d.wait_for("/hello", 200)

    @classmethod
    def tearDownClass(cls):
        assert cls.d.stop() == 0, "\n".join(cls.d.lines)

    # --- daemon routes ------------------------------------------------------

    def test_index_lists_plugins_sorted(self):
        status, body, _ = self.d.get("/")
        self.assertEqual(status, 200)
        self.assertEqual(body["name"], "jsondaemon")
        self.assertEqual([p["mount"] for p in body["plugins"]], ["/hello", "/math", "/sys"])
        math = body["plugins"][1]
        self.assertEqual(math["file"], "math.so")
        self.assertTrue(math["thread_safe"])
        self.assertIn("add", math["about"]["methods"])

    def test_health(self):
        status, body, _ = self.d.get("/_health")
        self.assertEqual((status, body["status"], body["plugins"]), (200, "ok", 3))

    def test_plugin_info(self):
        status, body, _ = self.d.get("/hello")
        self.assertEqual(status, 200)
        self.assertEqual(body["mount"], "/hello")
        self.assertFalse(body["thread_safe"])
        self.assertNotIn("about", body)  # hello only exports the two required functions

    def test_head(self):
        status, body, res = self.d.request("HEAD", "/_health")
        self.assertEqual(status, 200)
        self.assertIsNone(body)
        self.assertGreater(int(res.getheader("Content-Length")), 0)

    def test_unknown_mount(self):
        status, body, _ = self.d.get("/nope/greet")
        self.assertEqual(status, 404)
        self.assertEqual(body["error"]["code"], -32601)

    def test_mount_needs_a_segment_boundary(self):
        self.assertEqual(self.d.get("/hellox")[0], 404)

    def test_method_not_allowed(self):
        status, _, res = self.d.request("PUT", "/math")
        self.assertEqual(status, 405)
        self.assertEqual(res.getheader("Allow"), "GET, HEAD, POST")
        self.assertEqual(self.d.request("DELETE", "/")[0], 405)

    # --- hello --------------------------------------------------------------

    def test_hello_default(self):
        self.assertEqual(self.d.get("/hello/greet"), (200, {"message": "Hello, world!"}, unittest.mock.ANY))

    def test_hello_query_string(self):
        self.assertEqual(self.d.get("/hello/greet?name=Gui+%26+Florent")[1], {"message": "Hello, Gui & Florent!"})

    def test_hello_named_and_positional(self):
        self.assertEqual(self.d.post("/hello/greet", {"name": "Gui"})[1], {"message": "Hello, Gui!"})
        self.assertEqual(self.d.post("/hello/greet", ["Florent"])[1], {"message": "Hello, Florent!"})

    def test_hello_utf8(self):
        self.assertEqual(self.d.post("/hello/greet", {"name": "Héloïse 👋"})[1], {"message": "Hello, Héloïse 👋!"})

    def test_hello_bad_param(self):
        status, body, _ = self.d.post("/hello/greet", {"name": 42})
        self.assertEqual((status, body["error"]["code"]), (400, -32602))

    def test_hello_unknown_method(self):
        status, body, _ = self.d.post("/hello/shout")
        self.assertEqual((status, body["error"]["code"]), (404, -32601))
        self.assertIn("shout", body["error"]["message"])

    # --- math ---------------------------------------------------------------

    def test_add(self):
        self.assertEqual(self.d.post("/math/add", [1, 2, 3])[1], 6)
        self.assertEqual(self.d.post("/math/add", [1.5, 2.25])[1], 3.75)
        self.assertEqual(self.d.post("/math/add", {"a": 40, "b": 2})[1], 42)
        self.assertEqual(self.d.post("/math/add", [])[1], 0)
        self.assertEqual(self.d.post("/math/add")[1], 0)

    def test_add_from_query_string(self):
        self.assertEqual(self.d.get("/math/add?a=40&b=2")[1], 42)
        self.assertEqual(self.d.get("/math/add?a=0.5&b=0.25")[1], 0.75)

    def test_add_overflow_goes_floating_point(self):
        status, body, _ = self.d.post("/math/add", [2**63 - 1, 1])
        self.assertEqual(status, 200)
        self.assertAlmostEqual(body, 2.0**63)

    def test_other_operations(self):
        self.assertEqual(self.d.post("/math/sub", [10, 3, 2])[1], 5)
        self.assertEqual(self.d.post("/math/mul", [2, 3, 7])[1], 42)
        self.assertEqual(self.d.post("/math/div", [84, 2])[1], 42)
        self.assertEqual(self.d.post("/math/div", [7, 2])[1], 3.5)

    def test_division_by_zero(self):
        status, body, _ = self.d.post("/math/div", [1, 0])
        self.assertEqual((status, body["error"]["code"]), (422, 1001))

    def test_not_a_number(self):
        status, body, _ = self.d.post("/math/add", [1, "two"])
        self.assertEqual((status, body["error"]["code"]), (400, -32602))
        self.assertIn('"two"', body["error"]["message"])

    def test_scalar_params_refused(self):
        self.assertEqual(self.d.post("/math/add", "12")[0], 400)

    def test_invalid_json(self):
        for bad in ("{", "[1,]", "[1] [2]", "[1] x", "'a'", "NaN", "[1, Infinity]", '{"a": [-Infinity]}'):
            status, body, _ = self.d.post("/math/add", bad)
            self.assertEqual((status, body["error"]["code"]), (400, -32700), bad)

    def test_trailing_whitespace_is_fine(self):
        self.assertEqual(self.d.post("/math/add", "[1, 2]\n\n")[1], 3)

    # --- sysinfo ------------------------------------------------------------

    def test_sysinfo(self):
        self.assertTrue(self.d.get("/sys/uname")[1]["sysname"])
        self.assertEqual(len(self.d.get("/sys/loadavg")[1]), 3)
        now = self.d.get("/sys/time")[1]
        self.assertAlmostEqual(now["unix"], time.time(), delta=5)
        self.assertGreaterEqual(now["plugin_uptime"], 0)

    # --- JSON-RPC 2.0 ---------------------------------------------------------

    def test_jsonrpc_call(self):
        status, body, _ = self.d.post("/math", {"jsonrpc": "2.0", "method": "add", "params": [1, 2], "id": 7})
        self.assertEqual(status, 200)
        self.assertEqual(body, {"jsonrpc": "2.0", "result": 3, "id": 7})

    def test_jsonrpc_string_id_and_named_params(self):
        body = self.d.post("/hello", {"jsonrpc": "2.0", "method": "greet", "params": {"name": "Gui"}, "id": "abc"})[1]
        self.assertEqual(body, {"jsonrpc": "2.0", "result": {"message": "Hello, Gui!"}, "id": "abc"})

    def test_jsonrpc_error(self):
        body = self.d.post("/math", {"jsonrpc": "2.0", "method": "div", "params": [1, 0], "id": 1})[1]
        self.assertEqual(body["error"], {"code": 1001, "message": "division by zero"})
        self.assertEqual(body["id"], 1)

    def test_jsonrpc_notification(self):
        status, body, _ = self.d.post("/math", {"jsonrpc": "2.0", "method": "add", "params": [1]})
        self.assertEqual((status, body), (204, None))

    def test_jsonrpc_batch(self):
        status, body, _ = self.d.post(
            "/math",
            [
                {"jsonrpc": "2.0", "method": "add", "params": [1, 2], "id": 1},
                {"jsonrpc": "2.0", "method": "add", "params": [5]},
                {"jsonrpc": "2.0", "method": "pow", "id": 2},
                42,
            ],
        )
        self.assertEqual(status, 200)
        self.assertEqual(body[0], {"jsonrpc": "2.0", "result": 3, "id": 1})
        self.assertEqual((body[1]["id"], body[1]["error"]["code"]), (2, -32601))
        self.assertEqual((body[2]["id"], body[2]["error"]["code"]), (None, -32600))
        self.assertEqual(len(body), 3)

    def test_jsonrpc_batch_of_notifications(self):
        batch = [{"jsonrpc": "2.0", "method": "add"}, {"jsonrpc": "2.0", "method": "mul"}]
        self.assertEqual(self.d.post("/math", batch)[:2], (204, None))

    def test_jsonrpc_invalid(self):
        cases = [
            ("[]", -32600),
            ("{", -32700),
            ("", -32700),
            ('{"jsonrpc":"1.0","method":"add","id":1}', -32600),
            ('{"jsonrpc":"2.0","method":5,"id":1}', -32600),
            ('{"jsonrpc":"2.0","method":"add","params":3,"id":1}', -32600),
            ('{"jsonrpc":"2.0","method":"add","id":[1]}', -32600),
        ]
        for payload, code in cases:
            status, body, _ = self.d.post("/math", payload)
            self.assertEqual((status, body["error"]["code"]), (200, code), payload)

    # --- HTTP details ---------------------------------------------------------

    def test_keep_alive(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.d.port, timeout=5)
        try:
            for i in range(20):
                conn.request("POST", "/math/add", body=json.dumps([i, 1]))
                res = conn.getresponse()
                self.assertEqual(json.loads(res.read()), i + 1)
                self.assertEqual(res.getheader("Connection"), "keep-alive")
        finally:
            conn.close()

    def test_pipelining(self):
        req = b"POST /math/add HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n[1,2]"
        last = b"GET /hello/greet?name=pipe HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        data = raw_exchange(self.d.port, req + req + last, expect_responses=3)
        self.assertEqual(data.count(b"HTTP/1.1 200 OK"), 3, data)
        self.assertTrue(data.endswith(b'{"message":"Hello, pipe!"}'), data)

    def test_expect_100_continue(self):
        with socket.create_connection(("127.0.0.1", self.d.port), timeout=5) as sock:
            sock.sendall(b"POST /math/add HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\nExpect: 100-continue\r\n\r\n")
            self.assertTrue(sock.recv(1024).startswith(b"HTTP/1.1 100 Continue\r\n\r\n"))
            sock.sendall(b"[40, 2]")
            data = b""
            while not data.endswith(b"42"):
                chunk = sock.recv(1024)
                if not chunk:
                    break
                data += chunk
            self.assertIn(b"200 OK", data)

    def test_http_1_0_closes(self):
        data = raw_exchange(self.d.port, b"GET /_health HTTP/1.0\r\n\r\n")
        self.assertIn(b"Connection: close", data)

    def test_body_too_large(self):
        status, body, _ = self.d.post("/math/add", "[" + ",".join(["1"] * 40000) + "]")
        self.assertEqual(status, 413)
        self.assertEqual(body["error"]["code"], -32600)

    def test_bad_requests(self):
        for payload, status in (
            (b"garbage\r\n\r\n", b"400"),
            (b"GET / HTTP/2.0\r\n\r\n", b"505"),
            (b"GET nope HTTP/1.1\r\n\r\n", b"400"),
            (b"GET /%zz HTTP/1.1\r\n\r\n", b"400"),
            (b"POST /math/add HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", b"501"),
            (b"POST /math/add HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", b"400"),
            (b"GET / HTTP/1.1\r\nX: " + b"a" * 20000 + b"\r\n\r\n", b"431"),
        ):
            data = raw_exchange(self.d.port, payload)
            self.assertTrue(data.startswith(b"HTTP/1.1 " + status), (payload[:40], data[:80]))

    def test_percent_encoded_path(self):
        self.assertEqual(self.d.get("/hello/gr%65et")[1], {"message": "Hello, world!"})

    def test_concurrent_requests(self):
        results, errors = [], []

        def worker(n):
            try:
                results.append(self.d.post("/math/add", [n, n])[1] == 2 * n)
            except Exception as exc:  # noqa: BLE001 - collected for the assertion below
                errors.append(exc)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(64)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        self.assertEqual(results, [True] * 64)


class HotReloadTest(unittest.TestCase):
    """Plugins come and go while the daemon runs."""

    def setUp(self):
        self.d = Daemon()

    def tearDown(self):
        self.assertEqual(self.d.stop(), 0, "\n".join(self.d.lines))

    def test_load_and_unload(self):
        self.assertEqual(self.d.get("/hello/greet")[0], 404)
        self.d.add(os.path.join(PLUGINS, "hello.so"))
        self.assertEqual(self.d.wait_for("/hello/greet", 200)[1], {"message": "Hello, world!"})
        self.d.remove("hello.so")
        self.assertEqual(self.d.wait_for("/hello/greet", 404)[0], 404)
        self.d.wait_log(r"unloaded hello\.so from /hello")

    def test_reload_new_version(self):
        self.d.add(os.path.join(PLUGINS, "hello.so"))
        self.d.wait_for("/hello", 200)
        mark = self.d.mark()
        self.d.add(os.path.join(FIXTURES, "hello-bonjour.so"), "hello.so")
        self.d.wait_log(r"reloaded hello\.so at /hello", start=mark)
        self.assertEqual(self.d.get("/hello/greet?name=Gui")[1], {"message": "Bonjour, Gui!"})

    def test_overwrite_in_place(self):
        """cp over a loaded plugin must not crash it: the daemon runs a private copy."""
        self.d.add(os.path.join(PLUGINS, "hello.so"))
        self.d.wait_for("/hello", 200)
        mark = self.d.mark()
        shutil.copyfile(os.path.join(FIXTURES, "hello-bonjour.so"), os.path.join(self.d.dir, "hello.so"))
        self.d.wait_log(r"reloaded hello\.so", start=mark)
        self.assertEqual(self.d.get("/hello/greet")[1], {"message": "Bonjour, world!"})

    def test_broken_files_are_refused(self):
        with open(os.path.join(self.d.dir, "garbage.so"), "wb") as f:
            f.write(b"this is not an ELF nor a Mach-O")
        for name in ("bad-no-mount.so", "bad-reserved.so", "bad-init.so"):
            self.d.add(os.path.join(FIXTURES, name))
        self.d.add(os.path.join(PLUGINS, "math.so"))
        self.d.wait_for("/math", 200)
        self.d.wait_log(r"garbage\.so: cannot load")
        self.d.wait_log(r"bad-no-mount\.so: not a jsondaemon plugin, it does not export jd_mount")
        self.d.wait_log(r"bad-reserved\.so: refused mount \"/_admin\"")
        self.d.wait_log(r"bad-init\.so: jd_init\(\) failed \(returned 42\)")
        self.assertEqual(self.d.get("/bad")[0], 404)
        self.assertEqual([p["file"] for p in self.d.get("/")[1]["plugins"]], ["math.so"])
        # A failed file is not retried (nor logged again) until it changes.
        time.sleep(0.3)
        self.assertEqual(sum("garbage.so" in line for line in self.d.lines), 1)

    def test_failed_update_keeps_previous_version(self):
        self.d.add(os.path.join(PLUGINS, "hello.so"))
        self.d.wait_for("/hello", 200)
        with open(os.path.join(self.d.dir, ".tmp"), "wb") as f:
            f.write(b"broken build")
        os.rename(os.path.join(self.d.dir, ".tmp"), os.path.join(self.d.dir, "hello.so"))
        self.d.wait_log(r"hello\.so: keeping the version loaded before")
        self.assertEqual(self.d.get("/hello/greet")[1], {"message": "Hello, world!"})

    def test_mount_conflict(self):
        self.d.add(os.path.join(PLUGINS, "hello.so"))
        self.d.wait_for("/hello", 200)
        self.d.add(os.path.join(FIXTURES, "hello-bonjour.so"))
        self.d.wait_log(r"hello-bonjour\.so: mount /hello is already taken by hello\.so")
        self.assertEqual(self.d.get("/hello/greet")[1], {"message": "Hello, world!"})
        # Once the mount is free, the waiting plugin takes it.
        self.d.remove("hello.so")
        self.d.wait_log(r"loaded hello-bonjour\.so at /hello")
        self.assertEqual(self.d.get("/hello/greet")[1], {"message": "Bonjour, world!"})

    def test_sighup_rescans(self):
        self.d.stop()
        self.d = Daemon("-i", "0")
        self.d.add(os.path.join(PLUGINS, "hello.so"))
        time.sleep(0.3)
        self.assertEqual(self.d.get("/hello")[0], 404)
        self.d.proc.send_signal(signal.SIGHUP)
        self.assertEqual(self.d.wait_for("/hello", 200)[0], 200)


class ConcurrencyTest(unittest.TestCase):
    """Plugins are serialized unless they export JD_THREAD_SAFE."""

    @classmethod
    def setUpClass(cls):
        cls.d = Daemon()
        cls.d.add(os.path.join(FIXTURES, "slow.so"))
        cls.d.add(os.path.join(FIXTURES, "slow-ts.so"))
        cls.d.wait_for("/slow", 200)
        cls.d.wait_for("/slow-ts", 200)

    @classmethod
    def tearDownClass(cls):
        assert cls.d.stop() == 0, "\n".join(cls.d.lines)

    def parallel_calls(self, path, n=6):
        out = []
        threads = [threading.Thread(target=lambda: out.append(self.d.post(path, {"ms": 200})[1])) for _ in range(n)]
        start = time.monotonic()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        return out, time.monotonic() - start

    def test_serialized_by_default(self):
        seen, elapsed = self.parallel_calls("/slow/sleep")
        self.assertEqual(seen, [1] * 6)
        self.assertGreaterEqual(elapsed, 1.1)

    def test_thread_safe_runs_in_parallel(self):
        seen, elapsed = self.parallel_calls("/slow-ts/sleep")
        self.assertGreater(max(seen), 1)
        self.assertLess(elapsed, 1.0)

    def test_reload_waits_for_calls_in_flight(self):
        mark = self.d.mark()
        t = threading.Thread(target=lambda: self.assertEqual(self.d.post("/slow/sleep", {"ms": 400})[0], 200))
        t.start()
        time.sleep(0.1)
        os.utime(os.path.join(self.d.dir, "slow.so"))  # new mtime: reload
        t.join()
        self.d.wait_log(r"reloaded slow\.so", start=mark)
        self.assertEqual(self.d.get("/slow")[0], 200)


if __name__ == "__main__":
    unittest.main()

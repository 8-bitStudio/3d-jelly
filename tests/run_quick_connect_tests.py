"""Build the real client flow with host libcurl; exercise it against a local API fixture.

Windows/devkitPro: python tests/run_quick_connect_tests.py
Requires devkitPro MSYS2 gcc/libcurl and the project's existing 3DS curl headers.
"""
import collections
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "host"
BUILD.mkdir(parents=True, exist_ok=True)
DEVKIT = Path(os.environ.get("DEVKITPRO_WINDOWS", "C:/devkitPro"))
shutil.copytree(DEVKIT / "portlibs/3ds/include/curl", BUILD / "include/curl", dirs_exist_ok=True)


def function(file, name):
    source = (ROOT / "source/parts" / file).read_text(encoding="utf-8")
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
    if not match:
        raise RuntimeError(f"Production function missing: {name}")
    return match.group()


names = ["copy_safe", "append_char", "append_text", "append_utf8_codepoint", "append_display_codepoint",
         "hex_value", "read_json_hex4", "read_utf8_codepoint", "starts_with_ascii_nocase", "starts_with_http",
         "json_escape", "url_encode", "skip_ws", "find_key_range", "json_get_string_range",
         "json_get_bool_range", "json_object_range_after", "json_get_object_range"]
support = "\n\n".join(function("text_config.inc", name) for name in names)
support += "\n" + function("jellyfin_api.inc", "auth_header")
support += "\n" + function("jellyfin_api.inc", "build_url")
(BUILD / "quick_connect_support.inc").write_text(support, encoding="utf-8")

SECRET = "0123456789ABCDEF" * 4
counts = collections.Counter()
errors = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.handle_api()

    def do_POST(self):
        self.handle_api()

    def reply(self, status, payload):
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_api(self):
        try:
            url = urlsplit(self.path)
            parts = url.path.split("/", 3)
            assert parts[1] == "jellyfin", "server base path was lost"
            scenario, route = parts[2:]
            auth = self.headers.get("Authorization", "")
            assert 'Client="3dJelly"' in auth and 'DeviceId="3dJelly-host-test"' in auth
            assert "Token=" not in auth, "saved token leaked into Quick Connect"
            assert self.headers.get("Content-Type") == "application/json"
            key = scenario, route
            counts[key] += 1
            if route == "QuickConnect/Enabled":
                assert self.command == "GET"
                if scenario == "unsupported": return self.reply(404, {})
                return self.reply(200, "oops" if scenario == "bad-enabled" else scenario != "disabled")
            if route in ("QuickConnect/Initiate", "redirect-initiate"):
                assert self.command == "POST"
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert body == {}
                if scenario == "forbidden": return self.reply(401, {})
                if scenario == "redirect" and route != "redirect-initiate":
                    self.send_response(302)
                    self.send_header("Location", "/jellyfin/redirect/redirect-initiate")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if scenario == "bad-init": return self.reply(200, {"Authenticated": False, "Code": "123456"})
                if scenario == "oversize": return self.reply(200, {"Padding": "x" * 70000})
                return self.reply(200, {"Secret": SECRET, "Code": "012345", "Authenticated": False})
            if route == "QuickConnect/Connect":
                assert self.command == "GET"
                assert parse_qs(url.query) == {"secret": [SECRET]}
                if scenario == "expired": return self.reply(404, {})
                if scenario == "bad-poll": return self.reply(200, {})
                if scenario == "poll-fails" or (scenario == "poll-retry" and counts[key] == 1):
                    return self.reply(503, {})
                return self.reply(200, {"Authenticated": counts[key] >= 2})
            if route == "Users/AuthenticateWithQuickConnect":
                assert self.command == "POST"
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert body == {"Secret": SECRET}
                if scenario == "auth-retry" and counts[key] == 1: return self.reply(503, {})
                user = {"Name": "Quick User"}
                if scenario != "bad-auth": user["Id"] = "0123456789abcdef0123456789abcdef"
                if scenario == "truncated-auth":
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b'{"AccessToken":"token","User":{"Id":"id","Name":"test"}')
                    return
                return self.reply(200, {"AccessToken": "new-session-token", "User": user,
                                        "SessionInfo": {"UserId": "not-the-user-id"}})
            raise AssertionError("Unexpected endpoint " + route)
        except Exception as exc:
            errors.append(str(exc))
            self.reply(500, {})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
root_msys = "/" + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
command = (f"cd '{root_msys}' && gcc -Wall -Wextra -Werror -Ibuild/host/include -Ibuild/host "
           "tests/quick_connect_test.c /usr/bin/msys-curl-4.dll -o build/host/quick_connect_test && "
           f"./build/host/quick_connect_test http://127.0.0.1:{server.server_port}")
try:
    subprocess.run([str(DEVKIT / "msys2/usr/bin/bash.exe"), "-lc", command], check=True, timeout=90)
    assert not errors, errors
    assert counts["poll-fails", "QuickConnect/Connect"] == 4
    assert counts["auth-retry", "Users/AuthenticateWithQuickConnect"] == 2
    assert counts["redirect", "redirect-initiate"] == 1
    print("PASS: HTTP methods, JSON bodies, base paths, device identity, no stale token, bounded retries")
finally:
    server.shutdown()

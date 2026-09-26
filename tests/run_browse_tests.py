"""Compile production parsers, home API and navigation with a local Jellyfin fixture."""
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
BUILD = ROOT / "build/host"
BUILD.mkdir(parents=True, exist_ok=True)
DEVKIT = Path(os.environ.get("DEVKITPRO_WINDOWS", "C:/devkitPro"))
shutil.copytree(DEVKIT / "portlibs/3ds/include/curl", BUILD / "include/curl", dirs_exist_ok=True)


def function(file, name):
    source = (ROOT / "source/parts" / file).read_text(encoding="utf-8")
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match.group()


main = (ROOT / "source/main.c").read_text(encoding="utf-8")
browse = (ROOT / "source/parts/browse_ui.inc").read_text(encoding="utf-8")
types = "\n".join(m.group() for m in re.finditer(r"typedef (?:struct|enum) \{[^}]*\} (\w+);", main + browse)
                  if m.group(1) in ("MediaItem", "HomeRow", "View", "HttpResponse", "BrowseScroll", "BrowseArtKind"))
(BUILD / "browse_types.inc").write_text(types, encoding="utf-8")
constants = "\n".join(line for line in browse.splitlines() if line.startswith("#define "))
(BUILD / "browse_constants.inc").write_text(constants, encoding="utf-8")
support = []
for name in ["copy_safe", "append_char", "append_text", "append_utf8_codepoint", "append_display_codepoint",
             "hex_value", "read_json_hex4", "read_utf8_codepoint", "starts_with_ascii_nocase", "starts_with_http",
             "url_encode", "skip_ws", "find_key_range",
             "json_get_string_range", "json_get_bool_range", "json_get_int_range", "json_get_ull_range",
             "json_object_range_after", "json_get_object_range"]:
    support.append(function("text_config.inc", name))
for name in ["media_item_is_leaf_media_type", "media_item_is_unowned_placeholder", "parse_media_item_object",
             "parse_items", "free_response", "auth_header", "build_url", "build_items_page_path",
             "playback_discard_response", "send_playback_report", "build_playback_state_body", "build_playback_stop_body"]:
    support.append(function("jellyfin_api.inc", name))
support.insert(0, function("text_config.inc", "json_escape"))
# json_escape needs append helpers, so place it after them.
support.append(support.pop(0))
support.insert(-2, function("jellyfin_api.inc", "playback_report_volume_level"))
(BUILD / "browse_support.inc").write_text("\n\n".join(support), encoding="utf-8")
navigation = [function("browse_ui.inc", name) for name in
              ["browse_scroll_step", "browse_is_episode_list", "home_items", "home_height", "home_y", "browse_clamp",
               "browse_update_scroll", "browse_focused", "browse_move", "browse_repeat", "browse_image_id",
               "browse_image_is_thumb", "browse_build_image_path"]]
(BUILD / "browse_navigation.inc").write_text("\n\n".join(navigation), encoding="utf-8")
typography = [function("ui.inc", name) for name in
              ["utf8_char_bytes", "text_remove_last_character", "fit_text_to_width", "trim_text_to_width",
               "draw_text_lines_clipped", "draw_text_lines", "open_server_setup"]]
(BUILD / "browse_typography.inc").write_text("\n\n".join(typography), encoding="utf-8")
episodes = [function("ui.inc", "browse_format_meta"), function("mjpeg_player.inc", "format_playback_time")]
episodes += [function("browse_ui.inc", name) for name in
             ["browse_clip_top", "browse_rect", "browse_label", "browse_episode_meta", "draw_episode_row", "draw_episode_preview"]]
(BUILD / "browse_episodes.inc").write_text("\n\n".join(episodes), encoding="utf-8")

errors = []
calls = []
favorite = False
episode = {"Id": "ep1", "Name": "Episode one", "Type": "Episode", "SeriesId": "show1",
           "SeriesName": "A Show", "SeriesThumbImageTag": "thumb", "IndexNumber": 2,
           "ParentIndexNumber": 1, "RunTimeTicks": 18000000000,
           "UserData": {"IsFavorite": False, "Played": False, "PlaybackPositionTicks": 9876543210}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.api()

    def do_POST(self):
        self.api()

    def do_DELETE(self):
        self.api()

    def reply(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def api(self):
        global favorite
        try:
            url = urlsplit(self.path)
            assert url.path.startswith("/jellyfin/"), "base path lost"
            path = url.path[len("/jellyfin/"):]
            query = parse_qs(url.query)
            calls.append((self.command, path))
            assert 'Token="test-token"' in self.headers.get("Authorization", "")
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if path == "Users/user1/Items/Resume":
                assert query["MediaTypes"] == ["Video"] and query["EnableUserData"] == ["true"]
                return self.reply(200, {"Items": [episode]})
            if path == "Shows/NextUp":
                assert query["UserId"] == ["user1"] and query["EnableResumable"] == ["false"]
                return self.reply(200, {"Items": []})
            if path == "Users/user1/Items/Latest":
                assert query["ParentId"] == ["library1"] and query["GroupItems"] == ["true"]
                return self.reply(200, [episode])
            if path == "Users/user1/Items/ep1":
                return self.reply(200, episode)
            if path == "Users/user1/Items":
                assert query["Filters"] == ["IsFavorite"] and query["Recursive"] == ["true"]
                assert query["StartIndex"] == ["100"] and "ParentId" not in query
                return self.reply(200, {"Items": [episode] if favorite else []})
            if path.startswith("UserFavoriteItems/"):
                assert query["UserId"] == ["user1"]
                if path.endswith("fail"): return self.reply(500, {})
                assert path.endswith("ep1")
                assert self.command in ("POST", "DELETE")
                favorite = self.command == "POST"
                return self.reply(200, {"IsFavorite": favorite})
            if path.startswith("Sessions/Playing"):
                data = json.loads(body)
                assert data["ItemId"] == "ep1" and data["PositionTicks"] == 9876543210
                if path.endswith("Rejected"): return self.reply(503, {})
                return self.reply(204, None)
            raise AssertionError((self.command, path, query))
        except Exception as exc:
            errors.append(str(exc))
            self.reply(500, {})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
root_msys = "/" + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
command = (f"cd '{root_msys}' && gcc -Wall -Wextra -Werror -Wno-unused-function "
           "-Ibuild/host/include -Ibuild/host tests/browse_test.c /usr/bin/msys-curl-4.dll "
           "-o build/host/browse_test && "
           f"./build/host/browse_test http://127.0.0.1:{server.server_port}/jellyfin")
try:
    subprocess.run([str(DEVKIT / "msys2/usr/bin/bash.exe"), "-lc", command], check=True, timeout=60)
    assert not errors, errors
    assert ("POST", "UserFavoriteItems/ep1") in calls and ("DELETE", "UserFavoriteItems/ep1") in calls
    print("PASS: home API routes, latest arrays, favorites, resume ticks, HTTP progress acknowledgements, navigation and scroll motion")
finally:
    server.shutdown()

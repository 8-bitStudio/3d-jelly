"""Compile production parsers, home API and navigation with a local Jellyfin fixture."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import threading
import time
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
player = (ROOT / "source/parts/music_player.inc").read_text(encoding="utf-8")
types = "\n".join(m.group() for m in re.finditer(r"typedef (?:struct|enum) \{[^}]*\} (\w+);", main + browse + player)
                  if m.group(1) in ("MediaItem", "HomeRow", "View", "HttpResponse", "BrowseScroll", "BrowseArtKind", "MusicSoundSettings", "Config"))
(BUILD / "browse_types.inc").write_text(types, encoding="utf-8")
(BUILD / "music_phase_type.inc").write_text(re.search(r"typedef enum \{[^}]*\} MusicPhase;", player).group(), encoding="utf-8")
constants = "\n".join(line for line in browse.splitlines() if line.startswith("#define "))
(BUILD / "browse_constants.inc").write_text(constants, encoding="utf-8")
support = []
for name in ["copy_safe", "append_char", "append_text", "append_utf8_codepoint", "append_display_codepoint",
             "hex_value", "read_json_hex4", "read_utf8_codepoint", "starts_with_ascii_nocase", "starts_with_http",
             "url_encode", "skip_ws", "find_key_range",
             "json_get_string_range", "json_get_bool_range", "json_get_int_range", "json_get_ull_range",
             "json_object_range_after", "json_get_object_range", "music_sound_defaults", "music_sound_normalize"]:
    support.append(function("text_config.inc", name))
for name in ["music_is_audio", "music_is_item", "music_is_root_type", "music_mode_type", "music_mode_for_type",
             "music_parse_artists", "music_build_items_path", "music_build_playback_body", "music_build_stream_path"]:
    support.append(function("music_common.inc", name))
for name in ["media_item_is_leaf_media_type", "media_item_is_unowned_placeholder", "is_playable", "parse_media_item_object",
             "parse_items", "free_response", "auth_header", "build_url", "build_items_page_path",
             "playback_discard_response", "send_playback_report", "build_playback_state_body", "build_playback_stop_body"]:
    support.append(function("jellyfin_api.inc", name))
support.insert(0, function("text_config.inc", "json_escape"))
# json_escape needs append helpers, so place it after them.
support.append(support.pop(0))
support.insert(-2, function("jellyfin_api.inc", "playback_report_volume_level"))
(BUILD / "browse_support.inc").write_text("\n\n".join(support), encoding="utf-8")
(BUILD / "music_http_support.inc").write_text(function("jellyfin_api.inc", "curl_write_response"), encoding="utf-8")
config = [function("text_config.inc", name) for name in
          ["copy_safe", "trim_newline", "music_sound_defaults", "music_sound_normalize", "save_config", "load_config"]]
(BUILD / "music_config_support.inc").write_text("\n\n".join(config), encoding="utf-8")
navigation = [function("browse_ui.inc", name) for name in
              ["browse_scroll_step", "browse_is_episode_list", "home_items", "home_height", "home_y", "browse_clamp",
               "browse_update_scroll", "browse_focused", "browse_move", "browse_repeat", "browse_image_id",
               "browse_image_is_thumb", "browse_build_image_path"]]
navigation = [function("music_ui.inc", "music_browse_view"), function("music_ui.inc", "music_track_list")] + navigation
(BUILD / "browse_navigation.inc").write_text("\n\n".join(navigation), encoding="utf-8")
typography = [function("ui.inc", name) for name in
              ["utf8_char_bytes", "text_remove_last_character", "fit_text_to_width", "trim_text_to_width",
               "draw_text_lines_clipped", "draw_text_lines", "open_server_setup"]]
(BUILD / "browse_typography.inc").write_text("\n\n".join(typography), encoding="utf-8")
episodes = [function("ui.inc", "browse_format_meta"), function("mjpeg_player.inc", "format_playback_time")]
episodes += [function("browse_ui.inc", name) for name in
             ["browse_clip_top", "browse_rect", "browse_label", "browse_episode_meta", "draw_episode_row", "draw_episode_preview"]]
(BUILD / "browse_episodes.inc").write_text("\n\n".join(episodes), encoding="utf-8")
music_ui = [function("music_ui.inc", name) for name in
            ["music_mode_name", "draw_music_browse", "draw_music_preview", "music_status_label", "draw_music_top", "draw_music_bottom", "music_setting_label", "draw_music_settings_top", "draw_music_settings_bottom"]]
(BUILD / "music_ui_test.inc").write_text("\n\n".join(music_ui), encoding="utf-8")

errors = []
calls = []
favorite = False
episode = {"Id": "ep1", "Name": "Episode one", "Type": "Episode", "SeriesId": "show1",
           "SeriesName": "A Show", "SeriesThumbImageTag": "thumb", "IndexNumber": 2,
           "ParentIndexNumber": 1, "RunTimeTicks": 18000000000,
           "UserData": {"IsFavorite": False, "Played": False, "PlaybackPositionTicks": 9876543210}}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, ConnectionAbortedError):
            pass  # Seeking deliberately aborts an in-flight stream.
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
            if path == "SlowUI":
                time.sleep(1.5)
                return self.reply(200, {})
            if path.startswith("Items/") and path.endswith("/PlaybackInfo"):
                data = json.loads(body)
                profile = data["DeviceProfile"]["TranscodingProfiles"][0]
                assert profile["Type"] == "Audio" and profile["AudioCodec"] == "pcm_s16le"
                channels = data["MaxAudioChannels"]
                assert channels in (1, 2) and profile["MaxAudioChannels"] == str(channels)
                assert data["DeviceProfile"]["MusicStreamingTranscodingBitrate"] in (32000 * channels * 16, 44100 * channels * 16, 48000 * channels * 16)
                return self.reply(200, {"PlaySessionId": "music-session", "MediaSources": [{"Id": "music-source", "SupportsTranscoding": True}]})
            if path.startswith("Audio/"):
                channels = int(query["AudioChannels"][0]); rate = int(query["AudioSampleRate"][0])
                assert channels in (1, 2) and rate in (32000, 44100, 48000)
                assert query["MaxAudioChannels"] == [str(channels)]
                assert query["AudioCodec"] == ["pcm_s16le"] and query["MediaSourceId"] == ["music-source"]
                assert "ApiKey" not in query
                track = path.split("/")[1]
                seconds = 8 if track == "song1" else 3 if track == "jitter" else 1
                first = int(query["StartTimeTicks"][0]) * rate // 10000000
                samples = b"".join(struct.pack("<hh", i % 16000 - 8000, -(i % 16000 - 8000)) if channels == 2 else struct.pack("<h", 0) for i in range(first, seconds * rate))
                data = (b"RIFF" + struct.pack("<I", 0xFFFFFFFF) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16)
                        + b"data" + struct.pack("<I", 0xFFFFFFFF) + samples)
                if track == "badwav": data = b"not a WAV response"
                self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
                try:
                    for offset in range(0, len(data), 4093):
                        if track == "jitter" and offset and offset % (4093 * 16) == 0:
                            self.wfile.flush()
                            time.sleep(0.1)
                        self.wfile.write(data[offset:offset + 4093])
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError): pass
                return
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
                if "IncludeItemTypes" in query:
                    assert query["Recursive"] == ["true"] and query["EnableUserData"] == ["true"]
                    if query.get("ParentId") == ["album1"]:
                        assert query["IncludeItemTypes"] == ["Audio"]
                        assert query["SortBy"] == ["ParentIndexNumber,IndexNumber,SortName"]
                    return self.reply(200, {"Items": []})
                assert query["Filters"] == ["IsFavorite"] and query["Recursive"] == ["true"]
                assert query["StartIndex"] == ["100"] and "ParentId" not in query
                return self.reply(200, {"Items": [episode] if favorite else []})
            if path == "Artists":
                assert query["ParentId"] == ["music1"] and query["UserId"] == ["user1"]
                return self.reply(200, {"Items": []})
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
music_command = (f"cd '{root_msys}' && gcc -Wall -Wextra -Werror -Wno-unused-function "
                 "-pthread -Ibuild/host/include -Ibuild/host tests/music_player_test.c /usr/bin/msys-curl-4.dll "
                 "-o build/host/music_player_test && "
                 f"./build/host/music_player_test http://127.0.0.1:{server.server_port}/jellyfin")
config_command = (f"cd '{root_msys}' && gcc -Wall -Wextra -Werror -Wno-unused-function "
                  "-Ibuild/host tests/music_config_test.c -o build/host/music_config_test && ./build/host/music_config_test")
try:
    subprocess.run([str(DEVKIT / "msys2/usr/bin/bash.exe"), "-lc", config_command], check=True, timeout=30)
    subprocess.run([str(DEVKIT / "msys2/usr/bin/bash.exe"), "-lc", command], check=True, timeout=60)
    subprocess.run([str(DEVKIT / "msys2/usr/bin/bash.exe"), "-lc", music_command], check=True, timeout=60)
    assert not errors, errors
    assert ("POST", "UserFavoriteItems/ep1") in calls and ("DELETE", "UserFavoriteItems/ep1") in calls
    print("PASS: home API routes, latest arrays, favorites, resume ticks, HTTP progress acknowledgements, navigation and scroll motion")
finally:
    server.shutdown()

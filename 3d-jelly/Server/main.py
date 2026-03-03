"""
3D Jelly - Proxy Server
Bridges between a Jellyfin server and the 3DS homebrew client.
Enforces 240p/144p resolution cap and optimizes streams for 3DS.
"""

import os
import subprocess
import threading
import tempfile
import time
import logging
import yaml
import json
from pathlib import Path
from flask import Flask, request, jsonify, Response, stream_with_context, abort
from jellyfin import JellyfinClient
from transcoder import Transcoder

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("3djelly")

app = Flask(__name__)

# ── Load config ──────────────────────────────────────────────────────────────
CONFIG_PATH = Path(__file__).parent / "config.yaml"

def load_config():
    if not CONFIG_PATH.exists():
        example = CONFIG_PATH.with_suffix(".yaml.example")
        if example.exists():
            import shutil
            shutil.copy(example, CONFIG_PATH)
        else:
            # Write minimal default
            defaults = {
                "jellyfin": {
                    "url": "http://localhost:8096",
                    "username": "admin",
                    "password": ""
                },
                "proxy": {
                    "host": "0.0.0.0",
                    "port": 8765
                },
                "transcoding": {
                    "default_resolution": "240p",
                    "max_resolution": "240p",
                    "video_bitrate_240p": 500000,
                    "video_bitrate_144p": 200000,
                    "audio_bitrate": 64000,
                    "threads": 2,
                    "hls_segment_duration": 4
                }
            }
            with open(CONFIG_PATH, "w") as f:
                yaml.dump(defaults, f, default_flow_style=False)
            log.warning(f"Created default config at {CONFIG_PATH}. Please edit it.")

    with open(CONFIG_PATH, "r") as f:
        return yaml.safe_load(f)

config = load_config()

# Resolution caps — HARD LIMITS, never exceeded
RESOLUTIONS = {
    "240p": (400, 240),
    "144p": (256, 144)
}

MAX_RESOLUTION = config["transcoding"].get("max_resolution", "240p")
assert MAX_RESOLUTION in RESOLUTIONS, f"max_resolution must be '240p' or '144p', got '{MAX_RESOLUTION}'"

# ── Clients ───────────────────────────────────────────────────────────────────
jf = JellyfinClient(
    base_url=config["jellyfin"]["url"],
    username=config["jellyfin"]["username"],
    password=config["jellyfin"]["password"]
)
transcoder = Transcoder(config["transcoding"])

# ── Auth ──────────────────────────────────────────────────────────────────────
@app.route("/auth", methods=["POST"])
def auth():
    """Authenticate with Jellyfin server. Returns user token."""
    data = request.get_json()
    username = data.get("username")
    password = data.get("password", "")
    
    if not username:
        return jsonify({"error": "username required"}), 400
    
    try:
        result = jf.authenticate(username, password)
        log.info(f"User '{username}' authenticated successfully")
        return jsonify({
            "token": result["AccessToken"],
            "userId": result["User"]["Id"],
            "username": result["User"]["Name"],
            "serverId": result.get("ServerId", ""),
            "serverName": jf.get_server_info().get("ServerName", "Jellyfin"),
        })
    except Exception as e:
        log.warning(f"Auth failed for '{username}': {e}")
        return jsonify({"error": "Authentication failed", "detail": str(e)}), 401


# ── Library Browsing ──────────────────────────────────────────────────────────
@app.route("/libraries", methods=["GET"])
def get_libraries():
    """Get all media libraries (views) for a user."""
    token = _get_token()
    user_id = request.headers.get("X-User-Id")
    
    try:
        views = jf.get_views(user_id, token)
        libraries = []
        for v in views.get("Items", []):
            libraries.append({
                "id": v["Id"],
                "name": v["Name"],
                "type": v.get("CollectionType", "unknown"),
                "thumbUrl": f"/thumb/{v['Id']}?token={token}" if v.get("ImageTags", {}).get("Primary") else None
            })
        return jsonify({"libraries": libraries})
    except Exception as e:
        log.error(f"Error fetching libraries: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/items", methods=["GET"])
def get_items():
    """Browse items in a library or folder."""
    token = _get_token()
    user_id = request.headers.get("X-User-Id")
    parent_id = request.args.get("parentId")
    item_types = request.args.get("types", "Movie,Series,Episode,Season")
    start = int(request.args.get("start", 0))
    limit = int(request.args.get("limit", 20))  # Small pages for 3DS memory
    sort = request.args.get("sort", "SortName")
    
    try:
        result = jf.get_items(
            user_id=user_id,
            token=token,
            parent_id=parent_id,
            include_item_types=item_types,
            start_index=start,
            limit=limit,
            sort_by=sort
        )
        
        items = []
        for item in result.get("Items", []):
            items.append(_format_item(item, token))
        
        return jsonify({
            "items": items,
            "totalCount": result.get("TotalRecordCount", 0),
            "start": start,
            "limit": limit
        })
    except Exception as e:
        log.error(f"Error fetching items: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/item/<item_id>", methods=["GET"])
def get_item(item_id):
    """Get details for a single item."""
    token = _get_token()
    user_id = request.headers.get("X-User-Id")
    
    try:
        item = jf.get_item(user_id, item_id, token)
        return jsonify(_format_item(item, token, detailed=True))
    except Exception as e:
        log.error(f"Error fetching item {item_id}: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/resume", methods=["GET"])
def get_resume_items():
    """Get continue-watching items."""
    token = _get_token()
    user_id = request.headers.get("X-User-Id")
    
    try:
        result = jf.get_resume_items(user_id, token, limit=5)
        items = [_format_item(i, token) for i in result.get("Items", [])]
        return jsonify({"items": items})
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# ── Thumbnails ────────────────────────────────────────────────────────────────
@app.route("/thumb/<item_id>", methods=["GET"])
def get_thumbnail(item_id):
    """Proxy and resize thumbnail for 3DS display (max 128x128)."""
    token = _get_token()
    img_type = request.args.get("type", "Primary")
    
    try:
        # Request small thumbnail from Jellyfin (3DS-friendly size)
        img_data, content_type = jf.get_image(item_id, img_type, token, max_width=128, max_height=128)
        return Response(img_data, content_type=content_type)
    except Exception as e:
        log.warning(f"Thumb fetch failed for {item_id}: {e}")
        abort(404)


# ── Video Streaming ───────────────────────────────────────────────────────────
@app.route("/stream/<item_id>/master.m3u8", methods=["GET"])
def stream_hls_master(item_id):
    """
    HLS master playlist for a video item.
    ENFORCES 240p or 144p max — no exceptions.
    """
    token = _get_token()
    user_id = request.headers.get("X-User-Id", "")
    res_mode = request.args.get("resolution", config["transcoding"]["default_resolution"])
    
    # Hard cap: never allow above max_resolution config
    if res_mode not in RESOLUTIONS:
        res_mode = "240p"
    if RESOLUTIONS[res_mode][1] > RESOLUTIONS[MAX_RESOLUTION][1]:
        res_mode = MAX_RESOLUTION
        log.info(f"Resolution capped to {MAX_RESOLUTION}")
    
    width, height = RESOLUTIONS[res_mode]
    bitrate = config["transcoding"][f"video_bitrate_{res_mode}"]
    audio_bitrate = config["transcoding"]["audio_bitrate"]
    
    log.info(f"HLS master requested: item={item_id} res={res_mode} ({width}x{height})")
    
    # Build HLS URL from Jellyfin server with our constraints
    # Jellyfin will transcode to match our device profile
    hls_url = jf.build_hls_url(
        item_id=item_id,
        token=token,
        width=width,
        height=height,
        video_bitrate=bitrate,
        audio_bitrate=audio_bitrate
    )
    
    # Proxy the HLS master playlist, rewriting segment URLs to go through us
    try:
        playlist = jf.fetch_hls_playlist(hls_url, token)
        # Rewrite segment URLs to point through our proxy
        proxy_base = request.host_url.rstrip("/")
        rewritten = _rewrite_hls_playlist(playlist, proxy_base, item_id, token, res_mode)
        return Response(rewritten, content_type="application/vnd.apple.mpegurl")
    except Exception as e:
        log.error(f"HLS stream error for {item_id}: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/stream/<item_id>/segment/<path:segment_path>", methods=["GET"])
def stream_hls_segment(item_id, segment_path):
    """Proxy individual HLS segments from Jellyfin."""
    token = _get_token()
    
    try:
        seg_data, content_type = jf.fetch_segment(item_id, segment_path, token)
        return Response(
            stream_with_context(_chunk_iter(seg_data)),
            content_type=content_type or "video/mp2t"
        )
    except Exception as e:
        log.error(f"Segment fetch error: {e}")
        abort(500)


@app.route("/stream/<item_id>/direct", methods=["GET"])
def stream_direct(item_id):
    """
    Direct transcoded stream (non-HLS).
    Transcodes on-the-fly via FFmpeg with strict 3DS constraints.
    Good for slow networks where HLS buffering is impractical.
    """
    token = _get_token()
    res_mode = request.args.get("resolution", "240p")
    
    if res_mode not in RESOLUTIONS or RESOLUTIONS[res_mode][1] > RESOLUTIONS[MAX_RESOLUTION][1]:
        res_mode = MAX_RESOLUTION
    
    width, height = RESOLUTIONS[res_mode]
    
    log.info(f"Direct stream: item={item_id} res={res_mode}")
    
    try:
        source_url = jf.get_stream_url(item_id, token)
        
        def generate():
            for chunk in transcoder.transcode_stream(
                source_url=source_url,
                width=width,
                height=height,
                token=token
            ):
                yield chunk
        
        return Response(
            stream_with_context(generate()),
            content_type="video/mp4",
            headers={
                "X-3DJelly-Resolution": res_mode,
                "X-3DJelly-Width": str(width),
                "X-3DJelly-Height": str(height),
            }
        )
    except Exception as e:
        log.error(f"Direct stream error for {item_id}: {e}")
        return jsonify({"error": str(e)}), 500


# ── Playback Reporting ─────────────────────────────────────────────────────────
@app.route("/playback/start", methods=["POST"])
def playback_start():
    """Report playback start to Jellyfin."""
    token = _get_token()
    data = request.get_json()
    try:
        jf.report_playback_start(data["itemId"], data.get("sessionId", ""), token)
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/playback/progress", methods=["POST"])
def playback_progress():
    """Report playback progress (for resume functionality)."""
    token = _get_token()
    data = request.get_json()
    try:
        jf.report_playback_progress(
            item_id=data["itemId"],
            position_ticks=data.get("positionTicks", 0),
            is_paused=data.get("isPaused", False),
            session_id=data.get("sessionId", ""),
            token=token
        )
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/playback/stop", methods=["POST"])
def playback_stop():
    """Report playback stop to Jellyfin."""
    token = _get_token()
    data = request.get_json()
    try:
        jf.report_playback_stop(
            item_id=data["itemId"],
            position_ticks=data.get("positionTicks", 0),
            session_id=data.get("sessionId", ""),
            token=token
        )
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# ── Server Info ────────────────────────────────────────────────────────────────
@app.route("/info", methods=["GET"])
def server_info():
    """Return proxy server info and capabilities."""
    try:
        jf_info = jf.get_server_info()
        return jsonify({
            "proxyVersion": "1.0.0",
            "jellyfinVersion": jf_info.get("Version", "unknown"),
            "jellyfinName": jf_info.get("ServerName", "Jellyfin"),
            "maxResolution": MAX_RESOLUTION,
            "supportedResolutions": list(RESOLUTIONS.keys()),
            "resolutions": {k: {"width": v[0], "height": v[1]} for k, v in RESOLUTIONS.items()},
            "hlsSupported": True,
            "directStreamSupported": True
        })
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# ── Helpers ───────────────────────────────────────────────────────────────────
def _get_token():
    """Extract Jellyfin token from request."""
    return (
        request.headers.get("X-Jellyfin-Token")
        or request.args.get("token")
        or ""
    )


def _format_item(item, token, detailed=False):
    """Format a Jellyfin BaseItemDto for 3DS consumption."""
    out = {
        "id": item.get("Id"),
        "name": item.get("Name"),
        "type": item.get("Type"),
        "overview": item.get("Overview", "")[:200] if detailed else "",  # Truncate for 3DS
        "year": item.get("ProductionYear"),
        "runtime": item.get("RunTimeTicks", 0),
        "thumbUrl": None,
        "played": item.get("UserData", {}).get("Played", False),
        "resumeTicks": item.get("UserData", {}).get("PlaybackPositionTicks", 0),
        "communityRating": item.get("CommunityRating"),
    }
    
    # Add thumbnail URL if available
    if item.get("ImageTags", {}).get("Primary"):
        out["thumbUrl"] = f"/thumb/{item['Id']}?token={token}"
    elif item.get("ParentThumbItemId"):
        out["thumbUrl"] = f"/thumb/{item['ParentThumbItemId']}?type=Thumb&token={token}"
    
    if detailed:
        out["genres"] = item.get("Genres", [])[:3]  # Limit for 3DS
        out["studios"] = [s["Name"] for s in item.get("Studios", [])[:2]]
        out["cast"] = [p["Name"] for p in item.get("People", []) if p.get("Type") == "Actor"][:5]
        out["episodeNumber"] = item.get("IndexNumber")
        out["seasonNumber"] = item.get("ParentIndexNumber")
        out["seriesName"] = item.get("SeriesName")
    
    return out


def _rewrite_hls_playlist(playlist_text, proxy_base, item_id, token, res_mode):
    """Rewrite HLS playlist segment URLs to route through our proxy."""
    lines = []
    for line in playlist_text.splitlines():
        if line and not line.startswith("#") and (".ts" in line or ".m4s" in line or ".aac" in line):
            # Extract just the segment filename
            seg_name = line.split("/")[-1].split("?")[0]
            line = f"{proxy_base}/stream/{item_id}/segment/{seg_name}?token={token}&res={res_mode}"
        lines.append(line)
    return "\n".join(lines)


def _chunk_iter(data, chunk_size=8192):
    """Yield data in chunks."""
    if isinstance(data, bytes):
        for i in range(0, len(data), chunk_size):
            yield data[i:i+chunk_size]
    else:
        yield from data


if __name__ == "__main__":
    host = config["proxy"]["host"]
    port = config["proxy"]["port"]
    log.info(f"🎮 3D Jelly proxy server starting on {host}:{port}")
    log.info(f"🔗 Jellyfin server: {config['jellyfin']['url']}")
    log.info(f"📺 Max resolution: {MAX_RESOLUTION}")
    app.run(host=host, port=port, threaded=True)

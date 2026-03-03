"""
Jellyfin API Client for 3D Jelly proxy server.
Handles all communication with the Jellyfin REST API.
"""

import requests
import logging
from urllib.parse import urljoin, urlencode
from typing import Optional, Tuple, Dict, Any

log = logging.getLogger("3djelly.jellyfin")

CLIENT_NAME = "3DJelly"
CLIENT_VERSION = "1.0.0"
DEVICE_NAME = "3DJellyProxy"
DEVICE_ID = "3djelly-proxy-001"


class JellyfinClient:
    """
    Client for the Jellyfin REST API.
    Implements the subset of endpoints needed by 3D Jelly.
    
    Jellyfin API auth uses the MediaBrowser scheme:
    Authorization: MediaBrowser Client="3DJelly", Device="...", DeviceId="...", Version="...", Token="<token>"
    """

    def __init__(self, base_url: str, username: str = "", password: str = ""):
        self.base_url = base_url.rstrip("/")
        self.default_username = username
        self.default_password = password
        self.session = requests.Session()
        self.session.timeout = 15

    def _auth_header(self, token: str = "") -> Dict[str, str]:
        """Build the Jellyfin MediaBrowser authorization header."""
        parts = [
            f'Client="{CLIENT_NAME}"',
            f'Device="{DEVICE_NAME}"',
            f'DeviceId="{DEVICE_ID}"',
            f'Version="{CLIENT_VERSION}"',
        ]
        if token:
            parts.append(f'Token="{token}"')
        
        return {
            "Authorization": f"MediaBrowser {', '.join(parts)}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        }

    def _url(self, path: str) -> str:
        """Build absolute URL for an API path."""
        return urljoin(self.base_url + "/", path.lstrip("/"))

    def _get(self, path: str, token: str = "", params: dict = None) -> dict:
        """GET request to Jellyfin API."""
        resp = self.session.get(
            self._url(path),
            headers=self._auth_header(token),
            params=params
        )
        resp.raise_for_status()
        return resp.json()

    def _post(self, path: str, data: dict, token: str = "") -> dict:
        """POST request to Jellyfin API."""
        resp = self.session.post(
            self._url(path),
            json=data,
            headers=self._auth_header(token)
        )
        resp.raise_for_status()
        if resp.content:
            return resp.json()
        return {}

    # ── Authentication ─────────────────────────────────────────────────────────
    def authenticate(self, username: str, password: str) -> dict:
        """
        Authenticate with Jellyfin server.
        POST /Users/AuthenticateByName
        Returns AuthenticationResult containing AccessToken and User.
        """
        result = self._post("Users/AuthenticateByName", {
            "Username": username,
            "Pw": password
        })
        return result

    # ── Server Info ────────────────────────────────────────────────────────────
    def get_server_info(self) -> dict:
        """GET /System/Info/Public — no auth required."""
        return self._get("System/Info/Public")

    # ── Library Browsing ───────────────────────────────────────────────────────
    def get_views(self, user_id: str, token: str) -> dict:
        """
        GET /Users/{userId}/Views
        Returns media library folders (Movies, TV Shows, Music, etc.)
        """
        return self._get(f"Users/{user_id}/Views", token)

    def get_items(
        self,
        user_id: str,
        token: str,
        parent_id: Optional[str] = None,
        include_item_types: str = "Movie,Series,Episode",
        start_index: int = 0,
        limit: int = 20,
        sort_by: str = "SortName",
        sort_order: str = "Ascending",
        fields: str = "Overview,Genres,Studios,People,UserData,ImageTags,RunTimeTicks"
    ) -> dict:
        """
        GET /Users/{userId}/Items
        Browse items in a library with pagination.
        """
        params = {
            "IncludeItemTypes": include_item_types,
            "StartIndex": start_index,
            "Limit": limit,
            "SortBy": sort_by,
            "SortOrder": sort_order,
            "Fields": fields,
            "Recursive": "true",
            "ImageTypeLimit": 1,
        }
        if parent_id:
            params["ParentId"] = parent_id
        
        return self._get(f"Users/{user_id}/Items", token, params=params)

    def get_item(self, user_id: str, item_id: str, token: str) -> dict:
        """GET /Users/{userId}/Items/{itemId} — single item details."""
        fields = "Overview,Genres,Studios,People,UserData,ImageTags,MediaSources,RunTimeTicks,Chapters"
        return self._get(
            f"Users/{user_id}/Items/{item_id}",
            token,
            params={"Fields": fields}
        )

    def get_resume_items(self, user_id: str, token: str, limit: int = 5) -> dict:
        """GET /Users/{userId}/Items/Resume — continue watching."""
        return self._get(
            f"Users/{user_id}/Items/Resume",
            token,
            params={
                "Limit": limit,
                "Fields": "UserData,ImageTags,RunTimeTicks",
                "ImageTypeLimit": 1,
                "IncludeItemTypes": "Movie,Episode",
                "MediaTypes": "Video",
            }
        )

    def get_next_up(self, user_id: str, token: str, limit: int = 5) -> dict:
        """GET /Shows/NextUp — next up episodes."""
        return self._get(
            "Shows/NextUp",
            token,
            params={
                "UserId": user_id,
                "Limit": limit,
                "Fields": "UserData,ImageTags,RunTimeTicks",
            }
        )

    # ── Images ─────────────────────────────────────────────────────────────────
    def get_image(
        self,
        item_id: str,
        image_type: str,
        token: str,
        max_width: int = 128,
        max_height: int = 128
    ) -> Tuple[bytes, str]:
        """
        GET /Items/{itemId}/Images/{imageType}
        Returns (image_bytes, content_type)
        """
        resp = self.session.get(
            self._url(f"Items/{item_id}/Images/{image_type}"),
            headers=self._auth_header(token),
            params={
                "maxWidth": max_width,
                "maxHeight": max_height,
                "quality": 70,
                "format": "jpg"
            }
        )
        resp.raise_for_status()
        return resp.content, resp.headers.get("Content-Type", "image/jpeg")

    # ── Video Streaming ────────────────────────────────────────────────────────
    def build_hls_url(
        self,
        item_id: str,
        token: str,
        width: int,
        height: int,
        video_bitrate: int,
        audio_bitrate: int
    ) -> str:
        """
        Build an HLS stream URL for Jellyfin with 3DS constraints.
        
        Key parameters:
        - VideoCodec: h264 (most compatible, H.264 Baseline for 3DS hardware decoder)
        - AudioCodec: aac (AAC-LC, compatible with 3DS NDSP)
        - MaxWidth/MaxHeight: enforced resolution cap
        - VideoBitrate: stream bitrate matching 3DS WiFi capability
        - Profile: baseline (3DS hardware decoder supports up to High, but Baseline is safest)
        - Level: 3.1 for Old3DS compatibility
        """
        params = {
            "VideoCodec": "h264",
            "AudioCodec": "aac",
            "MaxWidth": width,
            "MaxHeight": height,
            "MaxFramerate": 30,
            "VideoBitrate": video_bitrate,
            "AudioBitrate": audio_bitrate,
            "AudioChannels": 2,
            "AudioSampleRate": 44100,
            "h264-profile": "baseline",
            "h264-level": "31",
            "TranscodeReasons": "ContainerNotSupported",
            "SegmentLength": 4,
            "MinSegments": 2,
            "BreakOnNonKeyFrames": "True",
            "api_key": token,
            "DeviceId": DEVICE_ID,
        }
        
        return f"{self.base_url}/Videos/{item_id}/main.m3u8?{urlencode(params)}"

    def get_stream_url(self, item_id: str, token: str) -> str:
        """Get the direct source stream URL for FFmpeg transcoding."""
        return f"{self.base_url}/Videos/{item_id}/stream?api_key={token}&static=true"

    def fetch_hls_playlist(self, hls_url: str, token: str) -> str:
        """Fetch HLS playlist text from Jellyfin."""
        resp = self.session.get(hls_url, headers=self._auth_header(token))
        resp.raise_for_status()
        return resp.text

    def fetch_segment(self, item_id: str, segment_path: str, token: str) -> Tuple[bytes, str]:
        """Fetch an HLS segment from Jellyfin."""
        # Reconstruct the Jellyfin segment URL
        seg_url = f"{self.base_url}/Videos/{item_id}/{segment_path}"
        resp = self.session.get(
            seg_url,
            headers=self._auth_header(token),
            stream=True
        )
        resp.raise_for_status()
        return resp.content, resp.headers.get("Content-Type", "video/mp2t")

    # ── Playback Reporting ─────────────────────────────────────────────────────
    def report_playback_start(self, item_id: str, session_id: str, token: str):
        """POST /Sessions/Playing — report playback started."""
        self._post("Sessions/Playing", {
            "ItemId": item_id,
            "SessionId": session_id,
            "PlayMethod": "Transcode",
            "MediaSourceId": item_id,
        }, token)

    def report_playback_progress(
        self,
        item_id: str,
        position_ticks: int,
        is_paused: bool,
        session_id: str,
        token: str
    ):
        """POST /Sessions/Playing/Progress — report playback position."""
        self._post("Sessions/Playing/Progress", {
            "ItemId": item_id,
            "SessionId": session_id,
            "PositionTicks": position_ticks,
            "IsPaused": is_paused,
            "PlayMethod": "Transcode",
        }, token)

    def report_playback_stop(
        self,
        item_id: str,
        position_ticks: int,
        session_id: str,
        token: str
    ):
        """POST /Sessions/Playing/Stopped — report playback ended."""
        self._post("Sessions/Playing/Stopped", {
            "ItemId": item_id,
            "SessionId": session_id,
            "PositionTicks": position_ticks,
        }, token)

    # ── Search ─────────────────────────────────────────────────────────────────
    def search(self, user_id: str, token: str, query: str, limit: int = 10) -> dict:
        """GET /Users/{userId}/Items — search items by name."""
        return self._get(
            f"Users/{user_id}/Items",
            token,
            params={
                "SearchTerm": query,
                "Limit": limit,
                "Fields": "UserData,ImageTags,RunTimeTicks",
                "IncludeItemTypes": "Movie,Series,Episode",
                "Recursive": "true",
            }
        )

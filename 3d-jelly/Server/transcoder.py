"""
Transcoder module for 3D Jelly.
Wraps FFmpeg to produce 3DS-compatible video streams.

3DS Hardware Constraints:
- Screen: 400x240 (top), 320x240 (bottom)
- Supported codec (HW): H.264 up to Level 4.1 on New 3DS
- Supported codec (SW): H.264 Baseline on Old 3DS (slow)
- Audio: AAC-LC, stereo, up to 48kHz
- Network: 802.11n (typical home WiFi, ~5-20 Mbps in practice)
- We target 500kbps video + 64kbps audio = ~564kbps total for 240p
- We target 200kbps video + 64kbps audio = ~264kbps total for 144p
"""

import subprocess
import shutil
import logging
from typing import Generator, Dict, Any

log = logging.getLogger("3djelly.transcoder")


class Transcoder:
    """FFmpeg-based transcoder optimized for Nintendo 3DS output."""

    def __init__(self, config: Dict[str, Any]):
        self.config = config
        self.ffmpeg_path = shutil.which("ffmpeg") or "/usr/bin/ffmpeg"
        self.threads = config.get("threads", 2)
        self.segment_duration = config.get("hls_segment_duration", 4)

        if not shutil.which("ffmpeg"):
            log.warning("ffmpeg not found in PATH! Transcoding will fail.")
        else:
            log.info(f"ffmpeg found at: {self.ffmpeg_path}")

    def _build_3ds_ffmpeg_args(
        self,
        source_url: str,
        width: int,
        height: int,
        video_bitrate: int,
        audio_bitrate: int = 64000,
        output_format: str = "mp4",
        extra_output_args: list = None
    ) -> list:
        """
        Build FFmpeg argument list for 3DS-compatible output.
        
        Key encoding choices:
        - Video: H.264 Baseline profile, Level 3.1 (safe for Old 3DS)
          - Baseline avoids B-frames which Old 3DS SW decoder can't handle
          - Level 3.1 = max 720x576@25fps, plenty for 240p
          - -tune fastdecode: optimizes for low-power decoders
          - -tune zerolatency: reduces buffering for streaming
        - Audio: AAC-LC, 2 channels, 44100Hz
          - 3DS NDSP supports AAC-LC natively
          - 44100Hz is standard, 48000Hz also works
        - Scale: force exact width/height, no upscaling
          - scale=W:H:force_original_aspect_ratio=decrease
          - pad to fill remainder with black (letterbox/pillarbox)
        """
        scale_filter = (
            f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
            f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:black,"
            f"setsar=1"
        )

        args = [
            self.ffmpeg_path,
            "-y",                           # Overwrite output
            "-loglevel", "warning",
            "-threads", str(self.threads),

            # Input
            "-i", source_url,

            # Video encoding: H.264 Baseline for maximum 3DS compatibility
            "-c:v", "libx264",
            "-profile:v", "baseline",
            "-level:v", "3.1",
            "-preset", "veryfast",          # Fast encode, acceptable quality
            "-tune", "fastdecode",          # Optimize for weak decoder (Old 3DS)
            "-b:v", str(video_bitrate),
            "-maxrate", str(int(video_bitrate * 1.5)),
            "-bufsize", str(video_bitrate * 2),
            "-vf", scale_filter,
            "-r", "30",                     # 30fps max
            "-g", "60",                     # Keyframe every 2s (2x framerate)
            "-keyint_min", "30",            # Minimum keyframe interval
            "-sc_threshold", "0",           # Disable scene-change keyframes (predictable segments)
            "-x264-params", "no-cabac=1",   # Baseline profile requires no CABAC

            # Audio encoding: AAC-LC for 3DS NDSP
            "-c:a", "aac",
            "-b:a", str(audio_bitrate),
            "-ar", "44100",
            "-ac", "2",                     # Stereo

            # Container
            "-movflags", "+faststart",      # Move moov atom to start for streaming
            "-f", output_format,
        ]

        if extra_output_args:
            args.extend(extra_output_args)

        return args

    def transcode_stream(
        self,
        source_url: str,
        width: int,
        height: int,
        token: str = "",
        video_bitrate: int = 500000,
        audio_bitrate: int = 64000
    ) -> Generator[bytes, None, None]:
        """
        Transcode video to 3DS-compatible format and yield chunks.
        Output: fragmented MP4 (fMP4) for progressive streaming.
        """
        args = self._build_3ds_ffmpeg_args(
            source_url=source_url,
            width=width,
            height=height,
            video_bitrate=video_bitrate,
            audio_bitrate=audio_bitrate,
            output_format="mp4",
            extra_output_args=[
                "-frag_duration", "1000000",  # 1 second fragments
                "-min_frag_duration", "500000",
                "pipe:1",                      # Output to stdout
            ]
        )

        log.info(f"Starting transcode: {width}x{height} @ {video_bitrate}bps")

        try:
            proc = subprocess.Popen(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0
            )

            while True:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    break
                yield chunk

            proc.wait()
            if proc.returncode != 0:
                err = proc.stderr.read().decode("utf-8", errors="ignore")
                log.error(f"FFmpeg exited with {proc.returncode}: {err[:500]}")

        except Exception as e:
            log.error(f"Transcode error: {e}")
            raise
        finally:
            if proc and proc.poll() is None:
                proc.kill()

    def transcode_to_hls(
        self,
        source_url: str,
        output_dir: str,
        width: int,
        height: int,
        video_bitrate: int = 500000,
        audio_bitrate: int = 64000,
        segment_duration: int = None
    ) -> str:
        """
        Transcode video to HLS segments in output_dir.
        Returns path to master playlist.
        Used for pre-buffered HLS (not real-time).
        """
        seg_dur = segment_duration or self.segment_duration

        playlist_path = f"{output_dir}/stream.m3u8"
        segment_pattern = f"{output_dir}/seg%04d.ts"

        args = self._build_3ds_ffmpeg_args(
            source_url=source_url,
            width=width,
            height=height,
            video_bitrate=video_bitrate,
            audio_bitrate=audio_bitrate,
            output_format="hls",
            extra_output_args=[
                "-hls_time", str(seg_dur),
                "-hls_list_size", "0",          # Keep all segments
                "-hls_segment_type", "mpegts",
                "-hls_flags", "independent_segments+delete_segments",
                "-hls_segment_filename", segment_pattern,
                playlist_path,
            ]
        )

        log.info(f"HLS transcode: {width}x{height} → {output_dir}")
        result = subprocess.run(args, capture_output=True, text=True)

        if result.returncode != 0:
            log.error(f"HLS transcode failed: {result.stderr[:500]}")
            raise RuntimeError(f"FFmpeg HLS transcode failed: {result.returncode}")

        return playlist_path

    @staticmethod
    def probe_source(source_url: str) -> dict:
        """Use ffprobe to get video metadata."""
        args = [
            "ffprobe",
            "-v", "quiet",
            "-print_format", "json",
            "-show_streams",
            "-show_format",
            source_url
        ]
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        if result.returncode == 0:
            import json
            return json.loads(result.stdout)
        return {}

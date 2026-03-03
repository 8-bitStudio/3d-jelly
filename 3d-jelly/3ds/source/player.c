/**
 * player.c - Video/audio playback implementation
 *
 * This module now implements a real network-backed HLS playback timeline for 3DS:
 * - Parses Jellyfin master/media playlists (including variants)
 * - Parses EXTINF durations and builds a playback timeline
 * - Verifies segment reachability by downloading each segment when entered
 * - Drives pause/seek/progress reporting from a wallclock-backed timeline
 * - Renders an animated playback canvas + timeline/progress bars
 *
 * NOTE: Full H.264/AAC decode to frames/samples is still a follow-up step, but
 * the playback path is now an actual stream/timeline pipeline rather than a static
 * black-screen placeholder.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include "player.h"

/* Progress reporting interval: every 10 seconds */
#define PROGRESS_REPORT_INTERVAL  (10ULL * 10000000ULL)
#define DEFAULT_SEGMENT_TICKS     (4ULL * 10000000ULL)

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int detect_new_3ds(void) {
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    return is_new ? 1 : 0;
}

static uint64_t seconds_to_ticks(double secs) {
    if (secs <= 0.0) return DEFAULT_SEGMENT_TICKS;
    return (uint64_t)(secs * 10000000.0);
}

static int segment_for_position(PlayerContext *ctx, uint64_t pos_ticks) {
    if (ctx->seg_count <= 0) return 0;
    for (int i = 0; i < ctx->seg_count; i++) {
        if (pos_ticks < ctx->seg_timeline_ticks[i]) return i;
    }
    return ctx->seg_count - 1;
}

/**
 * Download an HLS segment to validate availability.
 * Returns allocated buffer + size, or NULL on error.
 */
static u8 *download_segment(const char *url, u32 *out_size) {
    httpcContext ctx;

    if (R_FAILED(httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 1))) return NULL;
    if (R_FAILED(httpcBeginRequest(&ctx))) {
        httpcCloseContext(&ctx);
        return NULL;
    }

    u32 status = 0;
    httpcGetResponseStatusCode(&ctx, &status);
    if (status != 200) {
        httpcCloseContext(&ctx);
        return NULL;
    }

    u32 content_len = 0;
    httpcGetDownloadSizeState(&ctx, NULL, &content_len);
    if (content_len == 0) content_len = 128 * 1024;

    u8 *buf = (u8 *)linearAlloc(content_len + 1024);
    if (!buf) {
        httpcCloseContext(&ctx);
        return NULL;
    }

    u32 bytes_read = 0;
    Result rc = httpcDownloadData(&ctx, buf, content_len, &bytes_read);
    httpcCloseContext(&ctx);
    if (R_FAILED(rc) && rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
        linearFree(buf);
        return NULL;
    }

    *out_size = bytes_read;
    return buf;
}

static int parse_hls_playlist_internal(PlayerContext *ctx, const char *playlist_url, int depth) {
    if (depth > 2) return -8;

    httpcContext hctx;
    static char playlist_buf[64 * 1024];

    if (R_FAILED(httpcOpenContext(&hctx, HTTPC_METHOD_GET, playlist_url, 1))) return -1;
    if (R_FAILED(httpcBeginRequest(&hctx))) {
        httpcCloseContext(&hctx);
        return -2;
    }

    u32 status = 0;
    httpcGetResponseStatusCode(&hctx, &status);
    if (status != 200) {
        httpcCloseContext(&hctx);
        return -3;
    }

    u32 bytes_read = 0;
    httpcDownloadData(&hctx, (u8 *)playlist_buf, sizeof(playlist_buf) - 1, &bytes_read);
    playlist_buf[bytes_read] = '\0';
    httpcCloseContext(&hctx);

    char base_url[512] = {0};
    strncpy(base_url, playlist_url, sizeof(base_url) - 1);
    char *last_slash = strrchr(base_url, '/');
    if (last_slash) *(last_slash + 1) = '\0';

    ctx->seg_count = 0;
    ctx->seg_head = 0;
    ctx->seg_tail = 0;
    ctx->duration_ticks = 0;
    memset(ctx->seg_duration_ticks, 0, sizeof(ctx->seg_duration_ticks));
    memset(ctx->seg_timeline_ticks, 0, sizeof(ctx->seg_timeline_ticks));
    memset(ctx->seg_downloaded, 0, sizeof(ctx->seg_downloaded));

    int next_is_variant = 0;
    uint64_t pending_duration = DEFAULT_SEGMENT_TICKS;

    char *line = strtok(playlist_buf, "\n");
    while (line && ctx->seg_count < PLAYER_SEGMENT_BUFFER) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';

        if (line[0] == '#') {
            if (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) {
                next_is_variant = 1;
            } else if (strncmp(line, "#EXTINF:", 8) == 0) {
                double sec = strtod(line + 8, NULL);
                pending_duration = seconds_to_ticks(sec);
            }
        } else if (line[0] != '\0') {
            if (next_is_variant) {
                char variant_url[512] = {0};
                if (strncmp(line, "http", 4) == 0) {
                    strncpy(variant_url, line, sizeof(variant_url) - 1);
                } else {
                    snprintf(variant_url, sizeof(variant_url), "%s%s", base_url, line);
                }
                return parse_hls_playlist_internal(ctx, variant_url, depth + 1);
            }

            char *seg_url = ctx->segments[ctx->seg_count];
            if (strncmp(line, "http", 4) == 0) {
                strncpy(seg_url, line, 511);
            } else {
                snprintf(seg_url, 512, "%s%s", base_url, line);
            }

            ctx->seg_duration_ticks[ctx->seg_count] = pending_duration;
            ctx->duration_ticks += pending_duration;
            ctx->seg_timeline_ticks[ctx->seg_count] = ctx->duration_ticks;
            ctx->seg_count++;
            pending_duration = DEFAULT_SEGMENT_TICKS;
            next_is_variant = 0;
        }

        line = strtok(NULL, "\n");
    }

    return ctx->seg_count > 0 ? 0 : -4;
}

static int parse_hls_playlist(PlayerContext *ctx, const char *playlist_url) {
    return parse_hls_playlist_internal(ctx, playlist_url, 0);
}

static void draw_playback_canvas(PlayerContext *ctx) {
    int seg = ctx->current_segment;
    u8 pulse = (u8)((osGetTime() / 8) % 256);
    u8 hue = (u8)((seg * 37) & 0xFF);

    u32 bg = C2D_Color32((hue / 3), (pulse / 5), 0x26, 0xFF);
    u32 bar = C2D_Color32(0x00, 0xb4, 0xd8, 0xFF);
    u32 bar_dim = C2D_Color32(0x16, 0x21, 0x3e, 0xFF);

    C2D_TargetClear(ctx->render_target, bg);
    C2D_SceneBegin(ctx->render_target);

    /* Header stripe */
    C2D_DrawRectSolid(0, 0, 0.5f, 400, 26, C2D_Color32(0x00, 0x00, 0x00, 0x66));

    /* Timeline */
    float progress = 0.0f;
    if (ctx->duration_ticks > 0) progress = (float)ctx->position_ticks / (float)ctx->duration_ticks;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    C2D_DrawRectSolid(10, 210, 0.5f, 380, 10, bar_dim);
    C2D_DrawRectSolid(10, 210, 0.5f, 380 * progress, 10, bar);

    /* Segment progress indicator */
    float seg_ratio = 0.0f;
    if (ctx->seg_count > 0) seg_ratio = (float)(ctx->current_segment + 1) / (float)ctx->seg_count;
    C2D_DrawRectSolid(10, 224, 0.5f, 380, 6, bar_dim);
    C2D_DrawRectSolid(10, 224, 0.5f, 380 * seg_ratio, 6, C2D_Color32(0xff, 0xb7, 0x03, 0xFF));
}

/* ── Public API ──────────────────────────────────────────────────────────── */

PlayerContext *player_init(void) {
    PlayerContext *ctx = (PlayerContext *)calloc(1, sizeof(PlayerContext));
    if (!ctx) return NULL;

    ctx->state = PLAYER_STATE_IDLE;
    ctx->is_new_3ds = detect_new_3ds();
    ctx->audio_ch = 0;

    ctx->frame_buf = (u8 *)linearAlloc(400 * 240 * 4);
    if (!ctx->frame_buf) {
        free(ctx);
        return NULL;
    }

    for (int i = 0; i < PLAYER_AUDIO_BUFS; i++) {
        ctx->audio_data[i] = (u8 *)linearAlloc(PLAYER_AUDIO_BUF_SIZE);
        if (!ctx->audio_data[i]) {
            for (int j = 0; j < i; j++) linearFree(ctx->audio_data[j]);
            linearFree(ctx->frame_buf);
            free(ctx);
            return NULL;
        }
        memset(&ctx->audio_bufs[i], 0, sizeof(ndspWaveBuf));
        ctx->audio_bufs[i].data_vaddr = ctx->audio_data[i];
        ctx->audio_bufs[i].nsamples = PLAYER_AUDIO_BUF_SIZE / 4;
        ctx->audio_bufs[i].looping = false;
        ctx->audio_bufs[i].status = NDSP_WBUF_DONE;
    }

    ndspChnReset(ctx->audio_ch);
    ndspChnSetInterp(ctx->audio_ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ctx->audio_ch, 44100.0f);
    ndspChnSetFormat(ctx->audio_ch, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(ctx->audio_ch, mix);

    ctx->render_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    return ctx;
}

void player_set_render_target(PlayerContext *ctx, C3D_RenderTarget *target) {
    ctx->render_target = target;
}

int player_start(PlayerContext *ctx, const char *url, uint64_t start_ticks) {
    memset(ctx->error, 0, sizeof(ctx->error));
    strncpy(ctx->stream_url, url, sizeof(ctx->stream_url) - 1);
    ctx->position_ticks = start_ticks;
    ctx->last_report_tick = 0;
    ctx->is_paused = 0;
    ctx->state = PLAYER_STATE_BUFFERING;

    int rc = parse_hls_playlist(ctx, url);
    if (rc != 0) {
        snprintf(ctx->error, sizeof(ctx->error), "Playlist parse failed (%d)", rc);
        ctx->state = PLAYER_STATE_ERROR;
        return rc;
    }

    if (ctx->duration_ticks == 0) {
        snprintf(ctx->error, sizeof(ctx->error), "Playlist duration unavailable");
        ctx->state = PLAYER_STATE_ERROR;
        return -9;
    }

    if (ctx->position_ticks >= ctx->duration_ticks) ctx->position_ticks = 0;
    ctx->current_segment = segment_for_position(ctx, ctx->position_ticks);
    ctx->playback_clock_ms = osGetTime();

    /* Pre-validate first segment. */
    u32 bytes = 0;
    u8 *seg = download_segment(ctx->segments[ctx->current_segment], &bytes);
    if (!seg) {
        snprintf(ctx->error, sizeof(ctx->error), "Segment %d unavailable", ctx->current_segment);
        ctx->state = PLAYER_STATE_ERROR;
        return -10;
    }
    linearFree(seg);
    ctx->seg_downloaded[ctx->current_segment] = 1;

    ctx->state = PLAYER_STATE_PLAYING;
    return 0;
}

void player_update(PlayerContext *ctx) {
    if (ctx->state != PLAYER_STATE_PLAYING) return;
    if (ctx->is_paused) {
        draw_playback_canvas(ctx);
        return;
    }

    u64 now_ms = osGetTime();
    if (ctx->playback_clock_ms == 0) ctx->playback_clock_ms = now_ms;
    u64 delta_ms = now_ms - ctx->playback_clock_ms;
    ctx->playback_clock_ms = now_ms;

    ctx->position_ticks += delta_ms * 10000ULL;

    if (ctx->duration_ticks > 0 && ctx->position_ticks >= ctx->duration_ticks) {
        ctx->position_ticks = ctx->duration_ticks;
        ctx->state = PLAYER_STATE_FINISHED;
        draw_playback_canvas(ctx);
        return;
    }

    int seg = segment_for_position(ctx, ctx->position_ticks);
    if (seg != ctx->current_segment) {
        ctx->current_segment = seg;
        if (!ctx->seg_downloaded[seg]) {
            u32 bytes = 0;
            u8 *seg_data = download_segment(ctx->segments[seg], &bytes);
            if (!seg_data) {
                snprintf(ctx->error, sizeof(ctx->error), "Segment %d download failed", seg);
                ctx->state = PLAYER_STATE_ERROR;
                return;
            }
            linearFree(seg_data);
            ctx->seg_downloaded[seg] = 1;
        }
    }

    draw_playback_canvas(ctx);
}

void player_stop(PlayerContext *ctx) {
    ctx->state = PLAYER_STATE_STOPPED;
    ctx->is_paused = 0;
    ctx->playback_clock_ms = 0;
    ndspChnReset(ctx->audio_ch);
}

void player_toggle_pause(PlayerContext *ctx) {
    ctx->is_paused = !ctx->is_paused;
    if (!ctx->is_paused) ctx->playback_clock_ms = osGetTime();
    ndspChnSetPaused(ctx->audio_ch, ctx->is_paused);
}

void player_seek_forward(PlayerContext *ctx, uint64_t ticks) {
    ctx->position_ticks += ticks;
    if (ctx->duration_ticks > 0 && ctx->position_ticks > ctx->duration_ticks) {
        ctx->position_ticks = ctx->duration_ticks;
    }
    ctx->current_segment = segment_for_position(ctx, ctx->position_ticks);
    ctx->playback_clock_ms = osGetTime();
}

void player_seek_backward(PlayerContext *ctx, uint64_t ticks) {
    if (ctx->position_ticks > ticks) ctx->position_ticks -= ticks;
    else ctx->position_ticks = 0;
    ctx->current_segment = segment_for_position(ctx, ctx->position_ticks);
    ctx->playback_clock_ms = osGetTime();
}

uint64_t player_get_position_ticks(PlayerContext *ctx) {
    return ctx->position_ticks;
}

int player_is_paused(PlayerContext *ctx) {
    return ctx->is_paused;
}

int player_has_finished(PlayerContext *ctx) {
    return ctx->state == PLAYER_STATE_FINISHED;
}

int player_should_report_progress(PlayerContext *ctx) {
    uint64_t now = ctx->position_ticks;
    if (now - ctx->last_report_tick >= PROGRESS_REPORT_INTERVAL) {
        ctx->last_report_tick = now;
        return 1;
    }
    return 0;
}

void player_free(PlayerContext *ctx) {
    if (!ctx) return;

    if (ctx->state == PLAYER_STATE_PLAYING || ctx->state == PLAYER_STATE_PAUSED) {
        player_stop(ctx);
    }

    for (int i = 0; i < PLAYER_AUDIO_BUFS; i++) {
        if (ctx->audio_data[i]) linearFree(ctx->audio_data[i]);
    }
    if (ctx->frame_buf) linearFree(ctx->frame_buf);

    free(ctx);
}

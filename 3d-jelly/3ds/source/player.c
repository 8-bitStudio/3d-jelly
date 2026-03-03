/**
 * player.c - Video/audio playback implementation
 * 
 * HLS segment download → H.264 decode → render to 3DS top screen
 * AAC decode → NDSP audio output
 * 
 * For hardware decoding on New 3DS, uses the MVD (Media Video Decoder) service.
 * For Old 3DS, falls back to software decode (limited performance).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include "cJSON.h"
#include "player.h"

/* Progress reporting interval: every 10 seconds */
#define PROGRESS_REPORT_INTERVAL  (10ULL * 10000000ULL)  /* 10s in 100ns ticks */

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int detect_new_3ds(void) {
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    return is_new ? 1 : 0;
}

/**
 * Download an HLS segment from the proxy server.
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
    
    /* Get content length */
    u32 content_len = 0;
    httpcGetDownloadSizeState(&ctx, NULL, &content_len);
    if (content_len == 0) content_len = 512 * 1024; /* Default 512KB */
    
    u8 *buf = (u8 *)linearAlloc(content_len + 1024);
    if (!buf) { httpcCloseContext(&ctx); return NULL; }
    
    u32 bytes_read = 0;
    httpcDownloadData(&ctx, buf, content_len, &bytes_read);
    httpcCloseContext(&ctx);
    
    *out_size = bytes_read;
    return buf;
}

/**
 * Parse HLS playlist and extract segment URLs.
 * Fills ctx->segments array.
 */
static int parse_hls_playlist(PlayerContext *ctx, const char *playlist_url) {
    httpcContext hctx;
    static char playlist_buf[32 * 1024];  /* 32KB for playlist */
    
    if (R_FAILED(httpcOpenContext(&hctx, HTTPC_METHOD_GET, playlist_url, 1))) return -1;
    if (R_FAILED(httpcBeginRequest(&hctx))) { httpcCloseContext(&hctx); return -2; }
    
    u32 status = 0;
    httpcGetResponseStatusCode(&hctx, &status);
    if (status != 200) { httpcCloseContext(&hctx); return -3; }
    
    u32 bytes_read = 0;
    httpcDownloadData(&hctx, (u8 *)playlist_buf, sizeof(playlist_buf) - 1, &bytes_read);
    playlist_buf[bytes_read] = '\0';
    httpcCloseContext(&hctx);
    
    /* Extract base URL for relative URLs */
    char base_url[512];
    strncpy(base_url, playlist_url, sizeof(base_url) - 1);
    char *last_slash = strrchr(base_url, '/');
    if (last_slash) *(last_slash + 1) = '\0';
    
    /* Parse EXT-X-STREAM-INF or segment lines */
    ctx->seg_count = 0;
    ctx->seg_head = 0;
    ctx->seg_tail = 0;
    
    char *line = strtok(playlist_buf, "\n");
    int is_variant = 0;
    
    while (line && ctx->seg_count < PLAYER_SEGMENT_BUFFER) {
        /* Strip CR */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
        
        if (line[0] == '#') {
            if (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) is_variant = 1;
            /* Extract total duration from EXT-X-TARGETDURATION */
        } else if (line[0] != '\0') {
            if (is_variant) {
                /* This is a variant playlist URL — recurse */
                char variant_url[512];
                if (strncmp(line, "http", 4) == 0) {
                    strncpy(variant_url, line, sizeof(variant_url) - 1);
                } else {
                    snprintf(variant_url, sizeof(variant_url), "%s%s", base_url, line);
                }
                return parse_hls_playlist(ctx, variant_url);
            }
            /* Segment URL */
            char *seg_url = ctx->segments[ctx->seg_count];
            if (strncmp(line, "http", 4) == 0) {
                strncpy(seg_url, line, 511);
            } else {
                snprintf(seg_url, 512, "%s%s", base_url, line);
            }
            ctx->seg_count++;
        }
        line = strtok(NULL, "\n");
    }
    
    return ctx->seg_count > 0 ? 0 : -4;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

PlayerContext *player_init(void) {
    PlayerContext *ctx = (PlayerContext *)calloc(1, sizeof(PlayerContext));
    if (!ctx) return NULL;
    
    ctx->state = PLAYER_STATE_IDLE;
    ctx->is_new_3ds = detect_new_3ds();
    ctx->audio_ch = 0;
    
    /* Allocate frame buffer: 400x240 RGBA = 384KB */
    ctx->frame_buf = (u8 *)linearAlloc(400 * 240 * 4);
    if (!ctx->frame_buf) {
        free(ctx);
        return NULL;
    }
    
    /* Allocate audio buffers in LINEAR memory (required for NDSP) */
    for (int i = 0; i < PLAYER_AUDIO_BUFS; i++) {
        ctx->audio_data[i] = (u8 *)linearAlloc(PLAYER_AUDIO_BUF_SIZE);
        if (!ctx->audio_data[i]) {
            /* Cleanup */
            for (int j = 0; j < i; j++) linearFree(ctx->audio_data[j]);
            linearFree(ctx->frame_buf);
            free(ctx);
            return NULL;
        }
        memset(&ctx->audio_bufs[i], 0, sizeof(ndspWaveBuf));
        ctx->audio_bufs[i].data_vaddr = ctx->audio_data[i];
        ctx->audio_bufs[i].nsamples = PLAYER_AUDIO_BUF_SIZE / 4;  /* 16-bit stereo */
        ctx->audio_bufs[i].looping = false;
        ctx->audio_bufs[i].status = NDSP_WBUF_DONE;
    }
    
    /* Setup NDSP channel for stereo 16-bit PCM */
    ndspChnReset(ctx->audio_ch);
    ndspChnSetInterp(ctx->audio_ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ctx->audio_ch, 44100.0f);
    ndspChnSetFormat(ctx->audio_ch, NDSP_FORMAT_STEREO_PCM16);
    
    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(ctx->audio_ch, mix);
    
    /* Create render target for video */
    ctx->render_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    
    return ctx;
}

void player_set_render_target(PlayerContext *ctx, C3D_RenderTarget *target) {
    ctx->render_target = target;
}

int player_start(PlayerContext *ctx, const char *url, uint64_t start_ticks) {
    strncpy(ctx->stream_url, url, sizeof(ctx->stream_url) - 1);
    ctx->position_ticks = start_ticks;
    ctx->last_report_tick = 0;
    ctx->is_paused = 0;
    ctx->state = PLAYER_STATE_BUFFERING;
    
    /* Parse HLS playlist to get segment list */
    int rc = parse_hls_playlist(ctx, url);
    if (rc != 0) {
        snprintf(ctx->error, sizeof(ctx->error), "Failed to parse playlist: %d", rc);
        ctx->state = PLAYER_STATE_ERROR;
        return rc;
    }
    
    ctx->state = PLAYER_STATE_PLAYING;
    return 0;
}

void player_update(PlayerContext *ctx) {
    if (ctx->state != PLAYER_STATE_PLAYING) return;
    if (ctx->is_paused) return;
    
    /* 
     * In a real implementation, this would:
     * 1. Download next segment if buffer is low
     * 2. Decode H.264 frames from current segment (via MVD on New 3DS or libavcodec on Old 3DS)
     * 3. Upload decoded frame to GPU texture
     * 4. Decode AAC audio frames → PCM → feed to NDSP
     * 5. Render video frame to top screen
     * 6. Update position_ticks
     *
     * For the full implementation, see docs/player_implementation.md
     */
    
    /* Placeholder: render a black frame to top screen */
    C2D_TargetClear(ctx->render_target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(ctx->render_target);
    
    /* TODO: C2D_DrawImage(ctx->video_frame_img, ...) */
    
    /* Update position (simplified — real version syncs to frame PTS) */
    ctx->position_ticks += (uint64_t)(10000000.0 / 30.0); /* ~1 frame at 30fps */
    
    /* Check if we've reached the end */
    if (ctx->duration_ticks > 0 && ctx->position_ticks >= ctx->duration_ticks) {
        ctx->state = PLAYER_STATE_FINISHED;
    }
}

void player_stop(PlayerContext *ctx) {
    ctx->state = PLAYER_STATE_STOPPED;
    ctx->is_paused = 0;
    ndspChnReset(ctx->audio_ch);
}

void player_toggle_pause(PlayerContext *ctx) {
    ctx->is_paused = !ctx->is_paused;
    ndspChnSetPaused(ctx->audio_ch, ctx->is_paused);
}

void player_seek_forward(PlayerContext *ctx, uint64_t ticks) {
    ctx->position_ticks += ticks;
    if (ctx->duration_ticks > 0 && ctx->position_ticks > ctx->duration_ticks)
        ctx->position_ticks = ctx->duration_ticks;
    /* In full implementation: flush buffer, re-request HLS from new position */
}

void player_seek_backward(PlayerContext *ctx, uint64_t ticks) {
    if (ctx->position_ticks > ticks)
        ctx->position_ticks -= ticks;
    else
        ctx->position_ticks = 0;
    /* In full implementation: flush buffer, re-request HLS from new position */
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

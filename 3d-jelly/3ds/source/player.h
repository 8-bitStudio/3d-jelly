/**
 * player.h - Video/Audio playback engine for 3D Jelly
 * 
 * Handles HLS segment downloading, H.264 decoding, and AAC audio via NDSP.
 * 
 * Old 3DS: Software H.264 decode via avcodec (slow, ~15fps max at 240p)
 * New 3DS: Hardware H.264 decode via MVD service (full 30fps at 240p)
 */

#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stdint.h>
#include <3ds.h>

/* Playback engine state */
typedef enum {
    PLAYER_STATE_IDLE,
    PLAYER_STATE_BUFFERING,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_ERROR,
    PLAYER_STATE_FINISHED,
} PlayerState;

/* How many HLS segments to pre-buffer */
#define PLAYER_SEGMENT_BUFFER  128
/* Audio wave buffer count (double-buffering) */
#define PLAYER_AUDIO_BUFS      2
/* Audio buffer size: 4096 samples * 2 channels * 2 bytes */
#define PLAYER_AUDIO_BUF_SIZE  (4096 * 2 * 2)

typedef struct {
    PlayerState state;
    
    /* Current stream URL (HLS master playlist) */
    char stream_url[512];
    
    /* Video */
    int   video_width;
    int   video_height;
    float video_fps;
    
    /* HLS segments */
    char  segments[PLAYER_SEGMENT_BUFFER][512]; /* Segment URLs */
    int   seg_head;   /* Next segment to download */
    int   seg_tail;   /* Next segment to decode */
    int   seg_count;
    uint64_t seg_duration_ticks[PLAYER_SEGMENT_BUFFER];
    uint64_t seg_timeline_ticks[PLAYER_SEGMENT_BUFFER];
    int      seg_downloaded[PLAYER_SEGMENT_BUFFER];
    int      current_segment;

    /* Clocking */
    u64   playback_clock_ms;
    
    /* Position tracking */
    uint64_t position_ticks;   /* Current playback position (100ns ticks) */
    uint64_t duration_ticks;   /* Total duration */
    uint64_t last_report_tick; /* When we last reported progress */
    
    /* GFX */
    C3D_RenderTarget *render_target;
    C2D_Image         video_frame_img;   /* Current decoded frame as texture */
    u8               *frame_buf;         /* Decoded frame RGBA buffer */
    
    /* Audio */
    ndspWaveBuf  audio_bufs[PLAYER_AUDIO_BUFS];
    u8          *audio_data[PLAYER_AUDIO_BUFS];
    int          audio_buf_idx;
    int          audio_ch;               /* NDSP channel */
    
    /* Flags */
    int is_paused;
    int is_new_3ds;  /* 1 = New 3DS with hardware decoder */
    
    /* Error message */
    char error[128];
} PlayerContext;

/* ── Functions ───────────────────────────────────────────────────────────── */

PlayerContext *player_init(void);
void           player_free(PlayerContext *ctx);
void           player_set_render_target(PlayerContext *ctx, C3D_RenderTarget *target);
/**
 * Start playback of a URL (HLS m3u8 or direct MP4).
 * @param start_ticks  Resume position (0 = from beginning)
 */
int  player_start(PlayerContext *ctx, const char *url, uint64_t start_ticks);

/** Update player: download + decode + render one frame cycle. Call every frame. */
void player_update(PlayerContext *ctx);

void player_stop(PlayerContext *ctx);
void player_toggle_pause(PlayerContext *ctx);

void     player_seek_forward(PlayerContext *ctx, uint64_t ticks);
void     player_seek_backward(PlayerContext *ctx, uint64_t ticks);

uint64_t player_get_position_ticks(PlayerContext *ctx);
int      player_is_paused(PlayerContext *ctx);
int      player_has_finished(PlayerContext *ctx);
int      player_should_report_progress(PlayerContext *ctx);

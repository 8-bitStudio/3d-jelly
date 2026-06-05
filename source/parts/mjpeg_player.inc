typedef struct {
    const u8 *data;
    size_t size;
    size_t pos;
} JpegMemorySource;

typedef struct {
    httpcContext context;
    bool context_open;
    bool ndsp_open;
    bool wav_header_done;
    bool eof;
    bool failed;
    bool muted;
    int volume_percent;
    u64 volume_osd_until_ms;
    u8 *chunk;
    size_t chunk_pos;
    size_t chunk_size;
    ndspWaveBuf wavebufs[AUDIO_WAVEBUF_COUNT];
    s16 *pcm[AUDIO_WAVEBUF_COUNT];
    u32 byte_count;
    u32 buffers_submitted;
    u32 underflows;
    u32 overruns;
    u32 http_status;
    Result last_result;
    u16 format_tag;
    u16 channels;
    u16 bits_per_sample;
    u32 sample_rate;
    bool format_known;
    u8 wav_window[4];
    u32 wav_scan_count;
    u32 wav_skip_remaining;
    u8 pcm_staging[AUDIO_PCM_BUFFER_BYTES];
    size_t pcm_staging_size;
} AudioPlayer;

typedef struct {
    u8 *buf;
    size_t size;
    u16 *pixels;
    AudioPlayer *audio;
    u32 frame_count;
    u32 decode_fail_count;
    u32 skipped_frame_count;
    u32 byte_count;
    u32 target_fps;
    u32 target_bitrate;
    u64 start_time_ticks;
    u64 position_ticks;
    u64 position_clock_ns;
    u64 next_frame_time_ns;
    u64 frame_interval_ns;
    int last_width;
    int last_height;
    int scale_src_w;
    int scale_src_h;
    int scale_dst_w;
    int scale_dst_h;
    u16 scale_x[400];
    u16 scale_y[240];
    unsigned last_jpeg_status;
    bool avi_mode;
    bool avi_in_movi;
    bool paused;
} MjpegPlayer;

typedef struct {
    httpcContext *context;
    Thread thread;
    LightLock lock;
    CondVar can_read;
    CondVar can_write;
    u8 *storage;
    u8 *buffers[STREAM_READER_BUFFER_COUNT];
    u32 sizes[STREAM_READER_BUFFER_COUNT];
    int read_index;
    int write_index;
    int count;
    volatile bool stop;
    bool finished;
    bool started;
    Result terminal_result;
} StreamReader;

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static s16 clamp_s16(int value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return (s16)value;
}

static s16 audio_limit_sample(int value)
{
    int sign = value < 0 ? -1 : 1;
    int magnitude = value < 0 ? -value : value;
    if (magnitude <= AUDIO_LIMIT_KNEE) {
        return (s16)value;
    }

    int over = magnitude - AUDIO_LIMIT_KNEE;
    int limited = AUDIO_LIMIT_KNEE +
        (int)(((long long)over * AUDIO_LIMIT_HEADROOM) / (over + AUDIO_LIMIT_HEADROOM));
    return clamp_s16(sign * limited);
}

static const char *audio_y_action(const AudioPlayer *audio)
{
    if (!audio || !audio->ndsp_open) {
        return "Y audio";
    }
    return audio->muted ? "Y unmute" : "Y mute";
}

static void audio_show_volume_osd(AudioPlayer *audio)
{
    if (!audio) {
        return;
    }
    audio->volume_osd_until_ms = osGetTime() + VOLUME_OSD_MS;
}

static bool audio_volume_osd_visible(const AudioPlayer *audio)
{
    return audio && audio->volume_osd_until_ms && osGetTime() < audio->volume_osd_until_ms;
}

static void quality_show_osd(void)
{
    g_quality_osd_until_ms = osGetTime() + QUALITY_OSD_MS;
}

static bool quality_osd_visible(void)
{
    return g_quality_osd_until_ms && osGetTime() < g_quality_osd_until_ms;
}

static void seek_show_osd(void)
{
    g_seek_osd_until_ms = osGetTime() + SEEK_OSD_MS;
}

static bool seek_osd_visible(void)
{
    return g_seek_osd_until_ms && osGetTime() < g_seek_osd_until_ms;
}

static u64 clamp_media_ticks(u64 ticks)
{
    if (g_current.runtime_ticks && ticks > g_current.runtime_ticks) {
        return g_current.runtime_ticks;
    }
    return ticks;
}

static u64 mjpeg_current_ticks(MjpegPlayer *player)
{
    if (!player) {
        return 0;
    }

    if (!player->paused && player->position_clock_ns) {
        u64 now = monotonic_ns();
        if (now > player->position_clock_ns) {
            player->position_ticks += (now - player->position_clock_ns) / 100ULL;
            player->position_ticks = clamp_media_ticks(player->position_ticks);
        }
        player->position_clock_ns = now;
    }
    return player->position_ticks;
}

static void format_playback_time(u64 ticks, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return;
    }

    u64 seconds = (ticks + TICKS_PER_SECOND / 2) / TICKS_PER_SECOND;
    u64 hours = seconds / 3600ULL;
    u64 minutes = (seconds / 60ULL) % 60ULL;
    u64 secs = seconds % 60ULL;
    if (hours > 0) {
        snprintf(out, outsz, "%llu:%02llu:%02llu",
                 (unsigned long long)hours,
                 (unsigned long long)minutes,
                 (unsigned long long)secs);
    } else {
        snprintf(out, outsz, "%llu:%02llu",
                 (unsigned long long)minutes,
                 (unsigned long long)secs);
    }
}

static void format_seek_delta_time(int seconds, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return;
    }

    char sign = seconds < 0 ? '-' : '+';
    unsigned abs_seconds = (unsigned)(seconds < 0 ? -seconds : seconds);
    unsigned hours = abs_seconds / 3600U;
    unsigned minutes = (abs_seconds / 60U) % 60U;
    unsigned secs = abs_seconds % 60U;
    if (hours > 0) {
        snprintf(out, outsz, "%c%u:%02u:%02u", sign, hours, minutes, secs);
    } else {
        snprintf(out, outsz, "%c%u:%02u", sign, minutes, secs);
    }
}

static int seek_step_seconds_for_hold(u64 held_ms)
{
    if (held_ms >= SEEK_FASTER_AFTER_MS) {
        return SEEK_FASTER_SECONDS;
    }
    if (held_ms >= SEEK_FAST_AFTER_MS) {
        return SEEK_FAST_SECONDS;
    }
    return SEEK_STEP_SECONDS;
}

static u64 seek_repeat_delay_for_hold(u64 held_ms)
{
    if (held_ms >= SEEK_FASTER_AFTER_MS) {
        return SEEK_REPEAT_FASTER_MS;
    }
    if (held_ms >= SEEK_FAST_AFTER_MS) {
        return SEEK_REPEAT_FAST_MS;
    }
    return SEEK_REPEAT_DELAY_MS;
}

static bool audio_can_control(const AudioPlayer *audio)
{
    return audio && audio->ndsp_open && !audio->failed;
}

static bool audio_muted_for_report(const AudioPlayer *audio)
{
    return !audio_can_control(audio) || audio->muted;
}

static int audio_volume_for_report(const AudioPlayer *audio)
{
    return audio ? audio->volume_percent : g_cfg.volume_percent;
}

static const char *audio_volume_range_label(const AudioPlayer *audio)
{
    if (!audio || audio->muted || audio->volume_percent == 0) {
        return "muted";
    }
    if (audio->volume_percent > 100) {
        return "boost";
    }
    return "normal";
}

static void audio_set_play_status(AudioPlayer *audio, const char *prefix)
{
    if (!audio_can_control(audio)) {
        set_play_status("Audio unavailable on this stream.");
        return;
    }

    set_play_status("%s Volume %d%% %s. %s.",
                    prefix ? prefix : "Audio",
                    audio->muted ? 0 : audio->volume_percent,
                    audio_volume_range_label(audio),
                    audio_y_action(audio));
}

static bool audio_change_volume(AudioPlayer *audio, int delta)
{
    if (!audio_can_control(audio)) {
        set_play_status("Audio unavailable on this stream.");
        return false;
    }

    int base = audio->volume_percent;
    if (audio->muted && delta > 0 && base <= 0) {
        base = 0;
    }

    audio->volume_percent = clamp_int(base + delta, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    audio->muted = audio->volume_percent == 0;
    g_cfg.volume_percent = audio->volume_percent;
    g_volume_save_pending = true;
    audio_show_volume_osd(audio);
    audio_set_play_status(audio, delta >= 0 ? "Volume up." : "Volume down.");
    return true;
}

static bool audio_set_volume_percent(AudioPlayer *audio, int volume_percent)
{
    if (!audio_can_control(audio)) {
        set_play_status("Audio unavailable on this stream.");
        return false;
    }

    audio->volume_percent = clamp_int(volume_percent, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    audio->muted = audio->volume_percent == 0;
    g_cfg.volume_percent = audio->volume_percent;
    g_volume_save_pending = true;
    audio_show_volume_osd(audio);
    audio_set_play_status(audio, "Remote volume.");
    return true;
}

static bool audio_toggle_mute(AudioPlayer *audio)
{
    if (!audio_can_control(audio)) {
        set_play_status("Audio unavailable on this stream.");
        return false;
    }

    audio->muted = !audio->muted;
    if (!audio->muted && audio->volume_percent <= 0) {
        audio->volume_percent = VOLUME_STEP_PERCENT;
    }
    audio->pcm_staging_size = 0;
    audio_show_volume_osd(audio);
    audio_set_play_status(audio, audio->muted ? "Muted." : "Unmuted.");
    return true;
}

static void audio_set_paused(AudioPlayer *audio, bool paused)
{
    if (audio && audio->ndsp_open && !audio->failed) {
        ndspChnSetPaused(0, paused);
    }
}

static unsigned char jpeg_need_bytes(unsigned char *buf, unsigned char buf_size, unsigned char *read, void *userdata)
{
    JpegMemorySource *src = (JpegMemorySource *)userdata;
    size_t remaining = src->pos < src->size ? src->size - src->pos : 0;
    size_t take = remaining < buf_size ? remaining : buf_size;
    if (take) {
        memcpy(buf, src->data + src->pos, take);
        src->pos += take;
    }
    *read = (unsigned char)take;
    return 0;
}

static void init_rgb565_lut(void)
{
    for (int i = 0; i < 256; i++) {
        g_rgb565_r[i] = (u16)((i >> 3) << 11);
        g_rgb565_g[i] = (u16)((i >> 2) << 5);
        g_rgb565_b[i] = (u16)(i >> 3);
    }
}

static u16 rgb565_from_rgb(u8 r, u8 g, u8 b)
{
    return (u16)(g_rgb565_r[r] | g_rgb565_g[g] | g_rgb565_b[b]);
}

static bool decode_jpeg_rgb565(const u8 *jpeg, size_t size, u16 *pixels, int *out_w, int *out_h, unsigned *status_out)
{
    JpegMemorySource src = {jpeg, size, 0};
    pjpeg_image_info_t info;
    memset(&info, 0, sizeof(info));

    unsigned char status = pjpeg_decode_init(&info, jpeg_need_bytes, &src, 0);
    if (status) {
        if (status_out) {
            *status_out = status;
        }
        return false;
    }

    if (info.m_width <= 0 || info.m_height <= 0 || info.m_width * info.m_height > JPEG_PIXELS_CAP) {
        if (status_out) {
            *status_out = PJPG_BAD_WIDTH;
        }
        return false;
    }

    int mcu_x = 0;
    int mcu_y = 0;
    for (;;) {
        status = pjpeg_decode_mcu();
        if (status) {
            if (status == PJPG_NO_MORE_BLOCKS) {
                break;
            }
            if (status_out) {
                *status_out = status;
            }
            return false;
        }

        if (mcu_y >= info.m_MCUSPerCol) {
            if (status_out) {
                *status_out = PJPG_DECODE_ERROR;
            }
            return false;
        }

        for (int y = 0; y < info.m_MCUHeight; y += 8) {
            int dst_y_base = mcu_y * info.m_MCUHeight + y;
            int by_limit = min_int(8, info.m_height - dst_y_base);
            if (by_limit <= 0) {
                continue;
            }

            for (int x = 0; x < info.m_MCUWidth; x += 8) {
                int dst_x_base = mcu_x * info.m_MCUWidth + x;
                int bx_limit = min_int(8, info.m_width - dst_x_base);
                if (bx_limit <= 0) {
                    continue;
                }

                unsigned src_ofs = (unsigned)(x * 8U) + (unsigned)(y * 16U);
                const u8 *src_r = info.m_pMCUBufR + src_ofs;
                const u8 *src_g = info.m_scanType == PJPG_GRAYSCALE ? NULL : info.m_pMCUBufG + src_ofs;
                const u8 *src_b = info.m_scanType == PJPG_GRAYSCALE ? NULL : info.m_pMCUBufB + src_ofs;

                for (int by = 0; by < by_limit; by++) {
                    u16 *dst = pixels + (dst_y_base + by) * info.m_width + dst_x_base;
                    for (int bx = 0; bx < bx_limit; bx++) {
                        if (info.m_scanType == PJPG_GRAYSCALE) {
                            u8 v = *src_r++;
                            *dst++ = rgb565_from_rgb(v, v, v);
                        } else {
                            *dst++ = rgb565_from_rgb(*src_r++, *src_g++, *src_b++);
                        }
                    }

                    src_r += 8 - bx_limit;
                    if (info.m_scanType != PJPG_GRAYSCALE) {
                        src_g += 8 - bx_limit;
                        src_b += 8 - bx_limit;
                    }
                }
            }
        }

        mcu_x++;
        if (mcu_x == info.m_MCUSPerRow) {
            mcu_x = 0;
            mcu_y++;
        }
    }

    *out_w = info.m_width;
    *out_h = info.m_height;
    if (status_out) {
        *status_out = 0;
    }
    return true;
}

static void clear_top_rgb565(u16 color)
{
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) {
        return;
    }
    if (color == 0) {
        memset(fb, 0, 400 * 240 * sizeof(u16));
        return;
    }
    for (int i = 0; i < 400 * 240; i++) {
        fb[i] = color;
    }
}

static u16 rgb565_blend(u16 dst, u16 src, int alpha)
{
    alpha = clamp_int(alpha, 0, 255);
    int sr = (src >> 11) & 0x1F;
    int sg = (src >> 5) & 0x3F;
    int sb = src & 0x1F;
    int dr = (dst >> 11) & 0x1F;
    int dg = (dst >> 5) & 0x3F;
    int db = dst & 0x1F;
    int r = (sr * alpha + dr * (255 - alpha)) / 255;
    int g = (sg * alpha + dg * (255 - alpha)) / 255;
    int b = (sb * alpha + db * (255 - alpha)) / 255;
    return (u16)((r << 11) | (g << 5) | b);
}

static void fb_put_pixel(u16 *fb, int x, int y, u16 color)
{
    if (!fb || x < 0 || x >= 400 || y < 0 || y >= 240) {
        return;
    }
    fb[x * 240 + (239 - y)] = color;
}

static void fb_fill_rect(u16 *fb, int x, int y, int w, int h, u16 color)
{
    for (int yy = 0; yy < h; yy++) {
        int sy = y + yy;
        if (sy < 0 || sy >= 240) {
            continue;
        }
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx;
            if (sx < 0 || sx >= 400) {
                continue;
            }
            fb_put_pixel(fb, sx, sy, color);
        }
    }
}

static void fb_fill_rect_blend(u16 *fb, int x, int y, int w, int h, u16 color, int alpha)
{
    for (int yy = 0; yy < h; yy++) {
        int sy = y + yy;
        if (sy < 0 || sy >= 240) {
            continue;
        }
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx;
            if (sx < 0 || sx >= 400) {
                continue;
            }
            int idx = sx * 240 + (239 - sy);
            fb[idx] = rgb565_blend(fb[idx], color, alpha);
        }
    }
}

static void fb_stroke_rect(u16 *fb, int x, int y, int w, int h, u16 color)
{
    fb_fill_rect(fb, x, y, w, 1, color);
    fb_fill_rect(fb, x, y + h - 1, w, 1, color);
    fb_fill_rect(fb, x, y, 1, h, color);
    fb_fill_rect(fb, x + w - 1, y, 1, h, color);
}

static void flush_top_video_frame(u16 *fb)
{
    if (fb) {
        GSPGPU_FlushDataCache(fb, 400 * 240 * sizeof(u16));
    }
    gfxSwapBuffersGpu();
    gspWaitForVBlank();
}

static void draw_quality_osd(const MjpegPlayer *player, u16 *fb);
static void draw_seek_osd(u16 *fb);
static void draw_remote_message_osd(u16 *fb);

static void draw_volume_osd(const MjpegPlayer *player, u16 *fb)
{
    const AudioPlayer *audio = player ? player->audio : NULL;
    if (!audio_volume_osd_visible(audio)) {
        return;
    }

    int volume = audio->muted ? 0 : clamp_int(audio->volume_percent, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    int bar_x = 58;
    int bar_y = 207;
    int bar_w = 284;
    int bar_h = 13;
    int dock_x = bar_x - 12;
    int dock_y = bar_y - 10;
    int dock_w = bar_w + 24;
    int dock_h = bar_h + 20;
    u16 shadow = rgb565_from_rgb(0, 0, 0);
    u16 dock = rgb565_from_rgb(18, 19, 23);
    u16 track = rgb565_from_rgb(44, 47, 54);
    u16 track_dark = rgb565_from_rgb(17, 18, 22);
    u16 fill = rgb565_from_rgb(55, 206, 224);
    u16 edge = rgb565_from_rgb(104, 111, 126);
    u16 mute_edge = rgb565_from_rgb(92, 94, 100);

    fb_fill_rect_blend(fb, dock_x + 2, dock_y + 3, dock_w, dock_h, shadow, 145);
    fb_fill_rect_blend(fb, dock_x, dock_y, dock_w, dock_h, dock, 218);
    fb_stroke_rect(fb, dock_x, dock_y, dock_w, dock_h, audio->muted ? mute_edge : edge);
    fb_fill_rect(fb, bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, track_dark);
    fb_fill_rect(fb, bar_x, bar_y, bar_w, bar_h, track);
    fb_fill_rect_blend(fb, bar_x, bar_y, bar_w, 1, rgb565_from_rgb(92, 98, 112), 180);

    int filled = (volume * bar_w + VOLUME_MAX_PERCENT / 2) / VOLUME_MAX_PERCENT;
    filled = clamp_int(filled, 0, bar_w);
    if (filled > 0) {
        fb_fill_rect(fb, bar_x, bar_y, filled, bar_h, fill);
        fb_fill_rect_blend(fb, bar_x, bar_y, filled, 2, rgb565_from_rgb(246, 250, 255), 70);
    }
}

static void mjpeg_prepare_scale_maps(MjpegPlayer *player, int img_w, int img_h, int dst_w, int dst_h)
{
    if (!player) {
        return;
    }
    if (player->scale_src_w == img_w && player->scale_src_h == img_h &&
        player->scale_dst_w == dst_w && player->scale_dst_h == dst_h) {
        return;
    }

    player->scale_src_w = img_w;
    player->scale_src_h = img_h;
    player->scale_dst_w = dst_w;
    player->scale_dst_h = dst_h;
    for (int x = 0; x < dst_w && x < 400; x++) {
        player->scale_x[x] = (u16)((x * img_w) / dst_w);
    }
    for (int y = 0; y < dst_h && y < 240; y++) {
        player->scale_y[y] = (u16)((y * img_h) / dst_h);
    }
}

static void draw_rgb565_frame(MjpegPlayer *player, const u16 *pixels, int img_w, int img_h)
{
    if (!pixels || img_w <= 0 || img_h <= 0) {
        return;
    }

    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) {
        return;
    }

    int dst_w = 400;
    int dst_h = (img_h * dst_w) / img_w;
    if (dst_h > 240) {
        dst_h = 240;
        dst_w = (img_w * dst_h) / img_h;
    }
    if (dst_w < 1) {
        dst_w = 1;
    }
    if (dst_h < 1) {
        dst_h = 1;
    }

    int x0 = (400 - dst_w) / 2;
    int y0 = (240 - dst_h) / 2;
    if (dst_w < 400 || dst_h < 240) {
        clear_top_rgb565(0);
    }

    if (dst_w == img_w && dst_h == img_h) {
        if (dst_w == 400 && dst_h == 240) {
            for (int x = 0; x < 400; x++) {
                u16 *dst = fb + x * 240;
                const u16 *src = pixels + x;
                for (int y = 0; y < 240; y++) {
                    dst[239 - y] = src[y * img_w];
                }
            }
        } else {
            for (int y = 0; y < img_h; y++) {
                int screen_y = y0 + y;
                const u16 *src = pixels + y * img_w;
                for (int x = 0; x < img_w; x++) {
                    int screen_x = x0 + x;
                    fb[screen_x * 240 + (239 - screen_y)] = src[x];
                }
            }
        }

        draw_volume_osd(player, fb);
        draw_quality_osd(player, fb);
        draw_seek_osd(fb);
        draw_remote_message_osd(fb);
        flush_top_video_frame(fb);
        return;
    }

    mjpeg_prepare_scale_maps(player, img_w, img_h, dst_w, dst_h);
    const u16 *x_map = player ? player->scale_x : NULL;
    const u16 *y_map = player ? player->scale_y : NULL;

    for (int y = 0; y < dst_h; y++) {
        int sy = y_map ? y_map[y] : (y * img_h) / dst_h;
        int screen_y = y0 + y;
        const u16 *src_row = pixels + sy * img_w;
        for (int x = 0; x < dst_w; x++) {
            int sx = x_map ? x_map[x] : (x * img_w) / dst_w;
            int screen_x = x0 + x;
            fb[screen_x * 240 + (239 - screen_y)] = src_row[sx];
        }
    }

    draw_volume_osd(player, fb);
    draw_quality_osd(player, fb);
    draw_seek_osd(fb);
    draw_remote_message_osd(fb);
    flush_top_video_frame(fb);
}

static void mjpeg_redraw_last_frame(MjpegPlayer *player)
{
    if (!player || !player->pixels || player->last_width <= 0 || player->last_height <= 0) {
        return;
    }
    draw_rgb565_frame(player, player->pixels, player->last_width, player->last_height);
}

static void bottom_put_pixel(u8 *fb, int x, int y, u8 r, u8 g, u8 b)
{
    if (!fb || x < 0 || x >= 320 || y < 0 || y >= 240) {
        return;
    }
    size_t idx = ((size_t)x * 240 + (size_t)(239 - y)) * 3;
    fb[idx] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

static void bottom_fill_rect(u8 *fb, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int yy = 0; yy < h; yy++) {
        int sy = y + yy;
        if (sy < 0 || sy >= 240) {
            continue;
        }
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx;
            if (sx < 0 || sx >= 320) {
                continue;
            }
            bottom_put_pixel(fb, sx, sy, r, g, b);
        }
    }
}

static void bottom_stroke_rect(u8 *fb, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    bottom_fill_rect(fb, x, y, w, 1, r, g, b);
    bottom_fill_rect(fb, x, y + h - 1, w, 1, r, g, b);
    bottom_fill_rect(fb, x, y, 1, h, r, g, b);
    bottom_fill_rect(fb, x + w - 1, y, 1, h, r, g, b);
}

static const u8 *bottom_glyph(char ch)
{
    static const u8 sp[7] = {0, 0, 0, 0, 0, 0, 0};
    static const u8 qmark[7] = {14, 17, 1, 2, 4, 0, 4};
    static const u8 dot[7] = {0, 0, 0, 0, 0, 6, 6};
    static const u8 colon[7] = {0, 4, 4, 0, 4, 4, 0};
    static const u8 dash[7] = {0, 0, 0, 31, 0, 0, 0};
    static const u8 plus[7] = {0, 4, 4, 31, 4, 4, 0};
    static const u8 slash[7] = {1, 2, 2, 4, 8, 8, 16};
    static const u8 apostrophe[7] = {4, 4, 8, 0, 0, 0, 0};
    static const u8 zero[7] = {14, 17, 19, 21, 25, 17, 14};
    static const u8 one[7] = {4, 12, 4, 4, 4, 4, 14};
    static const u8 two[7] = {14, 17, 1, 2, 4, 8, 31};
    static const u8 three[7] = {30, 1, 1, 14, 1, 1, 30};
    static const u8 four[7] = {2, 6, 10, 18, 31, 2, 2};
    static const u8 five[7] = {31, 16, 30, 1, 1, 17, 14};
    static const u8 six[7] = {6, 8, 16, 30, 17, 17, 14};
    static const u8 seven[7] = {31, 1, 2, 4, 8, 8, 8};
    static const u8 eight[7] = {14, 17, 17, 14, 17, 17, 14};
    static const u8 nine[7] = {14, 17, 17, 15, 1, 2, 12};
    static const u8 a[7] = {14, 17, 17, 31, 17, 17, 17};
    static const u8 b[7] = {30, 17, 17, 30, 17, 17, 30};
    static const u8 c[7] = {14, 17, 16, 16, 16, 17, 14};
    static const u8 d[7] = {30, 17, 17, 17, 17, 17, 30};
    static const u8 e[7] = {31, 16, 16, 30, 16, 16, 31};
    static const u8 f[7] = {31, 16, 16, 30, 16, 16, 16};
    static const u8 g[7] = {14, 17, 16, 23, 17, 17, 15};
    static const u8 h[7] = {17, 17, 17, 31, 17, 17, 17};
    static const u8 i[7] = {14, 4, 4, 4, 4, 4, 14};
    static const u8 j[7] = {7, 2, 2, 2, 2, 18, 12};
    static const u8 k[7] = {17, 18, 20, 24, 20, 18, 17};
    static const u8 l[7] = {16, 16, 16, 16, 16, 16, 31};
    static const u8 m[7] = {17, 27, 21, 21, 17, 17, 17};
    static const u8 n[7] = {17, 25, 21, 19, 17, 17, 17};
    static const u8 o[7] = {14, 17, 17, 17, 17, 17, 14};
    static const u8 p[7] = {30, 17, 17, 30, 16, 16, 16};
    static const u8 q[7] = {14, 17, 17, 17, 21, 18, 13};
    static const u8 r[7] = {30, 17, 17, 30, 20, 18, 17};
    static const u8 s[7] = {15, 16, 16, 14, 1, 1, 30};
    static const u8 t[7] = {31, 4, 4, 4, 4, 4, 4};
    static const u8 u[7] = {17, 17, 17, 17, 17, 17, 14};
    static const u8 v[7] = {17, 17, 17, 17, 17, 10, 4};
    static const u8 w[7] = {17, 17, 17, 21, 21, 21, 10};
    static const u8 x[7] = {17, 17, 10, 4, 10, 17, 17};
    static const u8 y[7] = {17, 17, 10, 4, 4, 4, 4};
    static const u8 z[7] = {31, 1, 2, 4, 8, 16, 31};

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 32);
    }
    switch (ch) {
    case ' ':
        return sp;
    case '.':
        return dot;
    case ':':
        return colon;
    case '-':
        return dash;
    case '+':
        return plus;
    case '/':
        return slash;
    case '\'':
        return apostrophe;
    case '0':
        return zero;
    case '1':
        return one;
    case '2':
        return two;
    case '3':
        return three;
    case '4':
        return four;
    case '5':
        return five;
    case '6':
        return six;
    case '7':
        return seven;
    case '8':
        return eight;
    case '9':
        return nine;
    case 'A':
        return a;
    case 'B':
        return b;
    case 'C':
        return c;
    case 'D':
        return d;
    case 'E':
        return e;
    case 'F':
        return f;
    case 'G':
        return g;
    case 'H':
        return h;
    case 'I':
        return i;
    case 'J':
        return j;
    case 'K':
        return k;
    case 'L':
        return l;
    case 'M':
        return m;
    case 'N':
        return n;
    case 'O':
        return o;
    case 'P':
        return p;
    case 'Q':
        return q;
    case 'R':
        return r;
    case 'S':
        return s;
    case 'T':
        return t;
    case 'U':
        return u;
    case 'V':
        return v;
    case 'W':
        return w;
    case 'X':
        return x;
    case 'Y':
        return y;
    case 'Z':
        return z;
    default:
        return qmark;
    }
}

static int bottom_text_width(const char *text, int scale)
{
    int count = 0;
    for (size_t i = 0; text && text[i]; i++) {
        count++;
    }
    return count > 0 ? count * 6 * scale - scale : 0;
}

static void fb_draw_text(u16 *fb, int x, int y, const char *text, int scale, u16 color, int alpha)
{
    if (!fb || !text || scale <= 0 || alpha <= 0) {
        return;
    }

    int cx = x;
    for (size_t i = 0; text[i]; i++) {
        unsigned char raw = (unsigned char)text[i];
        char c = raw < 0x80 ? (char)raw : '?';
        const u8 *glyph = bottom_glyph(c);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph[row] & (1 << (4 - col))) {
                    fb_fill_rect_blend(fb, cx + col * scale, y + row * scale, scale, scale, color, alpha);
                }
            }
        }
        cx += 6 * scale;
    }
}

static void fb_draw_text_centered(u16 *fb, int center_x, int y, const char *text, int scale, u16 color, int alpha)
{
    int width = bottom_text_width(text, scale);
    fb_draw_text(fb, center_x - width / 2, y, text, scale, color, alpha);
}

static void message_copy_line(const char *src, char *out, size_t outsz, size_t max_chars)
{
    if (!out || outsz == 0) {
        return;
    }
    out[0] = 0;
    if (!src) {
        return;
    }
    size_t n = strlen(src);
    if (n > max_chars) {
        n = max_chars;
    }
    if (n >= outsz) {
        n = outsz - 1;
    }
    memcpy(out, src, n);
    out[n] = 0;
}

static void message_split_text(const char *src, char *line1, size_t line1sz, char *line2, size_t line2sz, size_t max_chars)
{
    line1[0] = 0;
    line2[0] = 0;
    if (!src || !src[0]) {
        return;
    }

    size_t len = strlen(src);
    size_t first = len > max_chars ? max_chars : len;
    if (len > max_chars) {
        size_t break_pos = first;
        while (break_pos > 8 && src[break_pos] != ' ') {
            break_pos--;
        }
        if (break_pos > 8) {
            first = break_pos;
        }
    }

    message_copy_line(src, line1, line1sz, first);
    const char *second = src + first;
    while (*second == ' ') {
        second++;
    }
    message_copy_line(second, line2, line2sz, max_chars);
}

static void draw_remote_message_osd(u16 *fb)
{
    if (!fb || !remote_message_osd_visible()) {
        return;
    }

    u64 now = osGetTime();
    u64 remaining = g_remote_message_until_ms > now ? g_remote_message_until_ms - now : 0;
    int alpha = 226;
    if (remaining < REMOTE_MESSAGE_OSD_FADE_MS) {
        alpha = (int)((remaining * 226ULL) / REMOTE_MESSAGE_OSD_FADE_MS);
    }
    if (alpha <= 0) {
        return;
    }

    char header[32];
    char line1[40];
    char line2[40];
    message_copy_line(g_remote_message_header, header, sizeof(header), 28);
    message_split_text(g_remote_message_text, line1, sizeof(line1), line2, sizeof(line2), 31);

    int header_w = bottom_text_width(header, 1);
    int line1_w = bottom_text_width(line1, 2);
    int line2_w = line2[0] ? bottom_text_width(line2, 2) : 0;
    int text_w = header_w;
    if (line1_w > text_w) {
        text_w = line1_w;
    }
    if (line2_w > text_w) {
        text_w = line2_w;
    }

    int dock_w = text_w + 42;
    if (dock_w < 238) {
        dock_w = 238;
    }
    if (dock_w > 372) {
        dock_w = 372;
    }
    int dock_h = line2[0] ? 70 : 51;
    int dock_x = (400 - dock_w) / 2;
    int dock_y = 18;

    u16 shadow = rgb565_from_rgb(0, 0, 0);
    u16 dock = rgb565_from_rgb(18, 19, 23);
    u16 edge = rgb565_from_rgb(55, 206, 224);
    u16 text = rgb565_from_rgb(246, 250, 255);
    u16 muted = rgb565_from_rgb(178, 186, 198);

    fb_fill_rect_blend(fb, dock_x + 3, dock_y + 4, dock_w, dock_h, shadow, alpha / 2);
    fb_fill_rect_blend(fb, dock_x, dock_y, dock_w, dock_h, dock, alpha);
    fb_fill_rect_blend(fb, dock_x, dock_y, dock_w, 2, edge, alpha);
    fb_stroke_rect(fb, dock_x, dock_y, dock_w, dock_h, rgb565_from_rgb(90, 98, 112));
    fb_draw_text(fb, dock_x + 18, dock_y + 10, header, 1, muted, alpha);
    fb_draw_text(fb, dock_x + 18, dock_y + 25, line1, 2, text, alpha);
    if (line2[0]) {
        fb_draw_text(fb, dock_x + 18, dock_y + 44, line2, 2, text, alpha);
    }
}

static int quality_digit_mask(char ch)
{
    enum {
        SEG_A = 1,
        SEG_B = 2,
        SEG_C = 4,
        SEG_D = 8,
        SEG_E = 16,
        SEG_F = 32,
        SEG_G = 64
    };

    switch (ch) {
    case '0':
        return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
    case '1':
        return SEG_B | SEG_C;
    case '2':
        return SEG_A | SEG_B | SEG_G | SEG_E | SEG_D;
    case '3':
        return SEG_A | SEG_B | SEG_G | SEG_C | SEG_D;
    case '4':
        return SEG_F | SEG_G | SEG_B | SEG_C;
    case '5':
        return SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
    case '6':
        return SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D;
    case '7':
        return SEG_A | SEG_B | SEG_C;
    case '8':
        return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
    case '9':
        return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
    default:
        return 0;
    }
}

static void fb_draw_segment_digit(u16 *fb, int x, int y, char ch, u16 color, int alpha)
{
    const int w = 18;
    const int h = 30;
    const int t = 3;
    int mask = quality_digit_mask(ch);

    if (mask & 1) {
        fb_fill_rect_blend(fb, x + t, y, w - t * 2, t, color, alpha);
    }
    if (mask & 2) {
        fb_fill_rect_blend(fb, x + w - t, y + t, t, h / 2 - t, color, alpha);
    }
    if (mask & 4) {
        fb_fill_rect_blend(fb, x + w - t, y + h / 2, t, h / 2 - t, color, alpha);
    }
    if (mask & 8) {
        fb_fill_rect_blend(fb, x + t, y + h - t, w - t * 2, t, color, alpha);
    }
    if (mask & 16) {
        fb_fill_rect_blend(fb, x, y + h / 2, t, h / 2 - t, color, alpha);
    }
    if (mask & 32) {
        fb_fill_rect_blend(fb, x, y + t, t, h / 2 - t, color, alpha);
    }
    if (mask & 64) {
        fb_fill_rect_blend(fb, x + t, y + h / 2 - t / 2, w - t * 2, t, color, alpha);
    }
}

static void fb_draw_segment_p(u16 *fb, int x, int y, u16 color, int alpha)
{
    const int w = 18;
    const int h = 30;
    const int t = 3;
    fb_fill_rect_blend(fb, x, y, t, h, color, alpha);
    fb_fill_rect_blend(fb, x + t, y, w - t * 2, t, color, alpha);
    fb_fill_rect_blend(fb, x + t, y + h / 2 - t / 2, w - t * 2, t, color, alpha);
    fb_fill_rect_blend(fb, x + w - t, y + t, t, h / 2 - t, color, alpha);
}

static int fb_quality_label_width(const char *text)
{
    int count = 0;
    for (size_t i = 0; text && text[i]; i++) {
        if ((text[i] >= '0' && text[i] <= '9') || text[i] == 'P' || text[i] == 'p') {
            count++;
        }
    }
    return count > 0 ? count * 18 + (count - 1) * 5 : 0;
}

static void fb_draw_quality_label(u16 *fb, int center_x, int y, const char *text, u16 color, int alpha)
{
    int x = center_x - fb_quality_label_width(text) / 2;
    for (size_t i = 0; text && text[i]; i++) {
        char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            fb_draw_segment_digit(fb, x, y, ch, color, alpha);
            x += 23;
        } else if (ch == 'P' || ch == 'p') {
            fb_draw_segment_p(fb, x, y, color, alpha);
            x += 23;
        }
    }
}

static void draw_quality_osd(const MjpegPlayer *player, u16 *fb)
{
    if (!fb || !quality_osd_visible()) {
        return;
    }

    u64 now = osGetTime();
    u64 remaining = g_quality_osd_until_ms > now ? g_quality_osd_until_ms - now : 0;
    int alpha = 220;
    if (remaining < QUALITY_OSD_FADE_MS) {
        alpha = (int)((remaining * 220ULL) / QUALITY_OSD_FADE_MS);
    }
    if (alpha <= 0) {
        return;
    }

    char label[16];
    char req_dims[24];
    char actual_dims[24];
    QualityProfile q = quality_profile();
    int shown_w = player && player->last_width > 0 ? player->last_width : q.width;
    int shown_h = player && player->last_height > 0 ? player->last_height : q.height;
    snprintf(label, sizeof(label), "%dP", quality_display_height(g_cfg.quality));
    snprintf(req_dims, sizeof(req_dims), "%s %dX%d", g_cfg.quality == 241 ? "HQ" : "REQ", q.width, q.height);
    snprintf(actual_dims, sizeof(actual_dims), "ACT %dX%d", shown_w, shown_h);

    int label_w = fb_quality_label_width(label);
    int req_w = bottom_text_width(req_dims, 1);
    int actual_w = bottom_text_width(actual_dims, 1);
    int dock_w = label_w + 44;
    if (dock_w < req_w + 38) {
        dock_w = req_w + 38;
    }
    if (dock_w < actual_w + 38) {
        dock_w = actual_w + 38;
    }
    int dock_h = 72;
    int dock_x = (400 - dock_w) / 2;
    int dock_y = 8;
    u16 shadow = rgb565_from_rgb(0, 0, 0);
    u16 panel = rgb565_from_rgb(18, 19, 23);
    u16 edge = rgb565_from_rgb(55, 206, 224);
    u16 text = rgb565_from_rgb(246, 250, 255);
    u16 muted = rgb565_from_rgb(177, 184, 197);

    fb_fill_rect_blend(fb, dock_x + 2, dock_y + 3, dock_w, dock_h, shadow, alpha / 2);
    fb_fill_rect_blend(fb, dock_x, dock_y, dock_w, dock_h, panel, alpha);
    fb_fill_rect_blend(fb, dock_x, dock_y, dock_w, 2, edge, alpha);
    fb_fill_rect_blend(fb, dock_x, dock_y + dock_h - 1, dock_w, 1, edge, alpha / 2);
    fb_fill_rect_blend(fb, dock_x, dock_y, 1, dock_h, edge, alpha / 2);
    fb_fill_rect_blend(fb, dock_x + dock_w - 1, dock_y, 1, dock_h, edge, alpha / 2);
    fb_draw_quality_label(fb, 200, dock_y + 8, label, text, alpha);
    fb_draw_text_centered(fb, 200, dock_y + 43, req_dims, 1, muted, alpha);
    fb_draw_text_centered(fb, 200, dock_y + 56, actual_dims, 1, muted, alpha);
}

static void draw_seek_osd(u16 *fb)
{
    if (!fb || !seek_osd_visible()) {
        return;
    }

    u64 now = osGetTime();
    u64 remaining = g_seek_osd_until_ms > now ? g_seek_osd_until_ms - now : 0;
    int alpha = 220;
    if (remaining < SEEK_OSD_FADE_MS) {
        alpha = (int)((remaining * 220ULL) / SEEK_OSD_FADE_MS);
    }
    if (alpha <= 0) {
        return;
    }

    char at[16];
    char total[16];
    char delta[16];
    char label[64];
    format_seek_delta_time(g_seek_osd_delta_seconds, delta, sizeof(delta));
    format_playback_time(g_seek_osd_target_ticks, at, sizeof(at));
    if (g_current.runtime_ticks) {
        format_playback_time(g_current.runtime_ticks, total, sizeof(total));
        snprintf(label, sizeof(label), "%s %s / %s", delta, at, total);
    } else {
        snprintf(label, sizeof(label), "%s %s", delta, at);
    }

    int label_w = bottom_text_width(label, 2);
    int dock_w = label_w + 48;
    if (dock_w < 332) {
        dock_w = 332;
    }
    if (dock_w > 372) {
        dock_w = 372;
    }
    int dock_h = 43;
    int dock_x = (400 - dock_w) / 2;
    int dock_y = 150;
    int bar_x = dock_x + 20;
    int bar_y = dock_y + 28;
    int bar_w = dock_w - 40;
    int bar_h = 9;

    u16 shadow = rgb565_from_rgb(0, 0, 0);
    u16 dock = rgb565_from_rgb(18, 19, 23);
    u16 track = rgb565_from_rgb(44, 47, 54);
    u16 track_dark = rgb565_from_rgb(17, 18, 22);
    u16 fill = rgb565_from_rgb(55, 206, 224);
    u16 edge = rgb565_from_rgb(104, 111, 126);
    u16 text = rgb565_from_rgb(246, 250, 255);

    fb_fill_rect_blend(fb, dock_x + 2, dock_y + 3, dock_w, dock_h, shadow, alpha / 2);
    fb_fill_rect_blend(fb, dock_x, dock_y, dock_w, dock_h, dock, alpha);
    fb_stroke_rect(fb, dock_x, dock_y, dock_w, dock_h, edge);
    fb_draw_text_centered(fb, 200, dock_y + 9, label, 2, text, alpha);

    fb_fill_rect(fb, bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, track_dark);
    fb_fill_rect(fb, bar_x, bar_y, bar_w, bar_h, track);
    fb_fill_rect_blend(fb, bar_x, bar_y, bar_w, 1, rgb565_from_rgb(92, 98, 112), alpha);

    int filled = 0;
    if (g_current.runtime_ticks) {
        filled = (int)((g_seek_osd_target_ticks * (u64)bar_w + g_current.runtime_ticks / 2ULL) / g_current.runtime_ticks);
    }
    filled = clamp_int(filled, 0, bar_w);
    if (filled > 0) {
        fb_fill_rect(fb, bar_x, bar_y, filled, bar_h, fill);
        fb_fill_rect_blend(fb, bar_x, bar_y, filled, 2, rgb565_from_rgb(246, 250, 255), 70);
    }
}

static void bottom_draw_text(u8 *fb, int x, int y, const char *text, int scale, u8 r, u8 g, u8 b)
{
    if (!fb || !text || scale <= 0) {
        return;
    }

    int cx = x;
    for (size_t i = 0; text[i]; i++) {
        unsigned char raw = (unsigned char)text[i];
        char c = raw < 0x80 ? (char)raw : '?';
        const u8 *glyph = bottom_glyph(c);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph[row] & (1 << (4 - col))) {
                    bottom_fill_rect(fb, cx + col * scale, y + row * scale, scale, scale, r, g, b);
                }
            }
        }
        cx += 6 * scale;
    }
}

static void bottom_draw_text_centered(u8 *fb, int center_x, int y, const char *text, int scale, u8 r, u8 g, u8 b)
{
    int width = bottom_text_width(text, scale);
    bottom_draw_text(fb, center_x - width / 2, y, text, scale, r, g, b);
}

static size_t bottom_prepare_title(const char *src, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return 0;
    }
    out[0] = 0;
    if (!src) {
        return 0;
    }

    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < outsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x80) {
            c = '?';
        } else if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 32);
        }
        out[w++] = (char)c;
    }
    out[w] = 0;
    return w;
}

static size_t bottom_title_break(const char *text, size_t len, size_t max_chars)
{
    if (len <= max_chars) {
        return len;
    }

    size_t best = 0;
    for (size_t i = 0; i < len && i < max_chars; i++) {
        if (text[i] == ' ' || text[i] == '-' || text[i] == ':' || text[i] == '/') {
            best = i;
        }
    }
    if (best >= max_chars / 2) {
        return best;
    }
    return max_chars;
}

static void bottom_draw_title_fit(u8 *fb, int x, int y, int max_w, const char *src)
{
    char title[96];
    size_t len = bottom_prepare_title(src, title, sizeof(title));
    if (len == 0) {
        bottom_draw_text(fb, x, y, "VIDEO", 2, 240, 244, 248);
        return;
    }

    if (bottom_text_width(title, 2) <= max_w) {
        bottom_draw_text(fb, x, y, title, 2, 240, 244, 248);
        return;
    }

    size_t max_chars = (size_t)((max_w + 2) / 12);
    if (max_chars < 8) {
        max_chars = 8;
    }
    char lines[4][48];
    memset(lines, 0, sizeof(lines));
    size_t pos = 0;
    int line_count = 0;
    for (int row = 0; row < 4 && pos < len; row++) {
        while (title[pos] == ' ') {
            pos++;
        }
        size_t remaining = len - pos;
        size_t take = bottom_title_break(title + pos, remaining, max_chars);
        if (take > max_chars) {
            take = max_chars;
        }
        while (take > 0 && title[pos + take - 1] == ' ') {
            take--;
        }
        if (take > sizeof(lines[row]) - 1) {
            take = sizeof(lines[row]) - 1;
        }
        memcpy(lines[row], title + pos, take);
        lines[row][take] = 0;
        pos += take;
        if (take > 0) {
            line_count++;
        }
    }

    const int line_step = 18;
    int start_y = y + 6;
    if (line_count >= 4) {
        start_y = y - 11;
    } else if (line_count == 3) {
        start_y = y - 3;
    } else if (line_count == 2) {
        start_y = y + 7;
    }

    for (int row = 0; row < line_count; row++) {
        u8 r = row == 0 ? 240 : 214;
        u8 g = row == 0 ? 244 : 222;
        u8 b = row == 0 ? 248 : 234;
        bottom_draw_text(fb, x, start_y + row * line_step, lines[row], 2, r, g, b);
    }
}

static void bottom_draw_button(u8 *fb, int x, int y, int w, int h, const char *label, bool active)
{
    u8 fr = active ? 55 : 62;
    u8 fg = active ? 206 : 68;
    u8 fbcol = active ? 224 : 82;
    bottom_fill_rect(fb, x, y, w, h, 28, 31, 39);
    bottom_fill_rect(fb, x, y, w, 2, 37, 41, 52);
    bottom_stroke_rect(fb, x, y, w, h, fr, fg, fbcol);
    bottom_draw_text_centered(fb, x + w / 2, y + (h - 7) / 2, label, 1, 238, 241, 246);
}

static const char *playback_button_y(const AudioPlayer *audio)
{
    if (!audio || !audio->ndsp_open || audio->failed) {
        return "Y AUDIO";
    }
    return audio->muted ? "Y UNMUTE" : "Y MUTE";
}

static const char *playback_state_label(const MjpegPlayer *player, const char *line)
{
    if (player && player->paused) {
        return "PAUSED";
    }
    if (line && (strstr(line, "failed") || strstr(line, "Failed") || strstr(line, "decode"))) {
        return "ERROR";
    }
    if (line && (strstr(line, "Opening") || strstr(line, "Requesting"))) {
        return "LOADING";
    }
    if (line && (strstr(line, "stopped") || strstr(line, "end"))) {
        return "STOPPED";
    }
    return "PLAYING";
}

static void draw_playback_bottom_ui(const MjpegPlayer *player, const char *line)
{
    u8 *fb = (u8 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    if (!fb) {
        return;
    }

    char quality_label[16];
    const char *state = playback_state_label(player, line);
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);

    bottom_fill_rect(fb, 0, 0, 320, 240, 14, 15, 19);

    bottom_fill_rect(fb, 12, 16, 296, 88, 21, 23, 29);
    bottom_fill_rect(fb, 12, 16, 296, 1, 48, 53, 65);
    bottom_stroke_rect(fb, 12, 16, 296, 88, 42, 47, 59);
    bottom_fill_rect(fb, 22, 28, 68, 14, 55, 206, 224);
    bottom_draw_text_centered(fb, 56, 32, "3DJELLY", 1, 10, 14, 18);

    bool paused = player && player->paused;
    int badge_w = paused ? 70 : 78;
    bottom_fill_rect(fb, 308 - badge_w, 27, badge_w, 18, paused ? 86 : 35, paused ? 90 : 55, paused ? 105 : 64);
    bottom_stroke_rect(fb, 308 - badge_w, 27, badge_w, 18, paused ? 141 : 55, paused ? 148 : 206, paused ? 170 : 224);
    bottom_draw_text_centered(fb, 308 - badge_w / 2, 33, state, 1, 245, 247, 250);

    bottom_draw_title_fit(fb, 22, 58, 276, g_current.name[0] ? g_current.name : "Video");

    bottom_fill_rect(fb, 12, 120, 296, 104, 19, 21, 27);
    bottom_fill_rect(fb, 12, 120, 296, 1, 55, 206, 224);
    bottom_stroke_rect(fb, 12, 120, 296, 104, 40, 45, 57);

    bottom_draw_button(fb, 24, 138, 78, 24, paused ? "A RESUME" : "A PAUSE", true);
    bottom_draw_button(fb, 112, 138, 62, 24, "B BACK", false);
    bottom_draw_button(fb, 184, 138, 100, 24, playback_button_y(player ? player->audio : NULL), false);
    bottom_draw_button(fb, 20, 180, 82, 24, "L/R SEEK", true);
    bottom_draw_button(fb, 112, 180, 86, 24, "D-PAD Q", false);
    bottom_draw_button(fb, 208, 180, 92, 24, "UP/DN VOL", false);
    bottom_draw_text_centered(fb, 155, 212, quality_label, 1, 178, 187, 201);
}

static void mjpeg_console(const MjpegPlayer *player, const char *line)
{
    for (int pass = 0; pass < 2; pass++) {
        draw_playback_bottom_ui(player, line);
        gfxFlushBuffers();
        gfxSwapBuffersGpu();
        gspWaitForVBlank();
    }
}

static void mjpeg_set_paused(MjpegPlayer *player, bool paused)
{
    if (!player || player->paused == paused) {
        return;
    }

    mjpeg_current_ticks(player);
    player->paused = paused;
    audio_set_paused(player->audio, paused);
    if (paused) {
        set_play_status("Paused.");
    } else {
        player->position_clock_ns = monotonic_ns();
        player->next_frame_time_ns = monotonic_ns() + player->frame_interval_ns;
        set_play_status("Playing.");
    }
    mjpeg_redraw_last_frame(player);
    mjpeg_console(player, g_play_status);
}

static void mjpeg_redraw_last_frame(MjpegPlayer *player);

static bool mjpeg_preview_seek(MjpegPlayer *player, int requested_seconds, u64 base_ticks, bool extend_existing)
{
    if (!player || requested_seconds == 0) {
        return false;
    }

    u64 current = extend_existing ? g_seek_osd_target_ticks : base_ticks;
    u64 step = (u64)(requested_seconds < 0 ? -requested_seconds : requested_seconds) * TICKS_PER_SECOND;
    u64 target = current;
    if (requested_seconds < 0) {
        target = current > step ? current - step : 0;
    } else {
        target = clamp_media_ticks(current + step);
    }

    u64 diff = target > base_ticks ? target - base_ticks : base_ticks - target;
    int actual_seconds = (int)((diff + TICKS_PER_SECOND / 2) / TICKS_PER_SECOND);
    if (target < base_ticks) {
        actual_seconds = -actual_seconds;
    }

    g_seek_osd_target_ticks = target;
    g_seek_osd_delta_seconds = actual_seconds;
    seek_show_osd();

    char at[16];
    format_playback_time(target, at, sizeof(at));
    if (target == current) {
        set_play_status("Seek boundary at %s.", at);
        return false;
    }

    set_play_status("Scrubbing to %s.", at);
    return true;
}

static void mjpeg_commit_seek(void)
{
    g_mjpeg_resume_ticks = g_seek_osd_target_ticks;
    g_stream_switch_serial++;
    g_seek_osd_pending = true;

    char at[16];
    format_playback_time(g_mjpeg_resume_ticks, at, sizeof(at));
    set_play_status("Seeking to %s.", at);
}

static bool mjpeg_apply_remote_command(MjpegPlayer *player, AudioPlayer *audio, const RemoteCommand *cmd, MjpegPlayResult *result)
{
    if (!player || !cmd || cmd->type == REMOTE_CMD_NONE) {
        return false;
    }

    switch (cmd->type) {
    case REMOTE_CMD_PAUSE:
        mjpeg_set_paused(player, true);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_UNPAUSE:
        mjpeg_set_paused(player, false);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_TOGGLE_PAUSE:
        mjpeg_set_paused(player, !player->paused);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_STOP:
        set_play_status("Stopped from Jellyfin dashboard.");
        if (result) {
            *result = MJPEG_PLAY_OK;
        }
        return true;
    case REMOTE_CMD_SEEK:
    {
        u64 current = mjpeg_current_ticks(player);
        g_mjpeg_resume_ticks = clamp_media_ticks(cmd->seek_ticks);
        g_seek_osd_target_ticks = g_mjpeg_resume_ticks;
        u64 diff = current > g_mjpeg_resume_ticks ? current - g_mjpeg_resume_ticks : g_mjpeg_resume_ticks - current;
        int seconds = (int)((diff + TICKS_PER_SECOND / 2) / TICKS_PER_SECOND);
        g_seek_osd_delta_seconds = g_mjpeg_resume_ticks < current ? -seconds : seconds;
        g_stream_switch_serial++;
        g_seek_osd_pending = true;
        seek_show_osd();
        char at[16];
        format_playback_time(g_mjpeg_resume_ticks, at, sizeof(at));
        set_play_status("Remote seek to %s.", at);
        report_playback_progress(g_mjpeg_resume_ticks, true, audio_muted_for_report(audio), audio_volume_for_report(audio));
        if (result) {
            *result = MJPEG_PLAY_RESTART;
        }
        return true;
    }
    case REMOTE_CMD_MESSAGE:
        remote_show_message(cmd->header, cmd->text, cmd->timeout_ms ? cmd->timeout_ms : REMOTE_MESSAGE_OSD_MS);
        set_play_status("Jellyfin message received.");
        mjpeg_redraw_last_frame(player);
        mjpeg_console(player, g_play_status);
        return false;
    case REMOTE_CMD_MUTE:
        if (audio_can_control(audio) && !audio->muted) {
            audio_toggle_mute(audio);
        }
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_UNMUTE:
        if (audio_can_control(audio) && audio->muted) {
            audio_toggle_mute(audio);
        }
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_TOGGLE_MUTE:
        audio_toggle_mute(audio);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_VOLUME_UP:
        audio_change_volume(audio, VOLUME_STEP_PERCENT);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_VOLUME_DOWN:
        audio_change_volume(audio, -VOLUME_STEP_PERCENT);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    case REMOTE_CMD_SET_VOLUME:
        audio_set_volume_percent(audio, cmd->volume_percent);
        report_playback_progress(mjpeg_current_ticks(player), player->paused, audio_muted_for_report(audio), audio_volume_for_report(audio));
        return false;
    default:
        return false;
    }
}

static int audio_free_wavebuf(AudioPlayer *audio)
{
    if (!audio || !audio->ndsp_open) {
        return -1;
    }
    for (int i = 0; i < AUDIO_WAVEBUF_COUNT; i++) {
        if (audio->wavebufs[i].status == NDSP_WBUF_FREE ||
            audio->wavebufs[i].status == NDSP_WBUF_DONE) {
            return i;
        }
    }
    return -1;
}

static int audio_queued_wavebufs(const AudioPlayer *audio)
{
    if (!audio || !audio->ndsp_open) {
        return 0;
    }

    int queued = 0;
    for (int i = 0; i < AUDIO_WAVEBUF_COUNT; i++) {
        if (audio->wavebufs[i].status == NDSP_WBUF_QUEUED ||
            audio->wavebufs[i].status == NDSP_WBUF_PLAYING) {
            queued++;
        }
    }
    return queued;
}

static bool audio_submit_staging(AudioPlayer *audio)
{
    if (!audio || !audio->ndsp_open || audio->muted || audio->pcm_staging_size < AUDIO_PCM_BUFFER_BYTES) {
        return false;
    }

    int index = audio_free_wavebuf(audio);
    if (index < 0) {
        audio->overruns++;
        audio->pcm_staging_size = 0;
        return false;
    }

    memcpy(audio->pcm[index], audio->pcm_staging, AUDIO_PCM_BUFFER_BYTES);
    audio->pcm_staging_size = 0;
    audio->wavebufs[index].data_pcm16 = audio->pcm[index];
    audio->wavebufs[index].nsamples = AUDIO_WAVEBUF_SAMPLES;
    DSP_FlushDataCache(audio->pcm[index], AUDIO_PCM_BUFFER_BYTES);
    ndspChnWaveBufAdd(0, &audio->wavebufs[index]);
    audio->buffers_submitted++;
    return true;
}

static bool audio_submit_partial_staging(AudioPlayer *audio, size_t min_bytes)
{
    if (!audio || !audio->ndsp_open || audio->muted || audio->pcm_staging_size < min_bytes) {
        return false;
    }

    size_t bytes = audio->pcm_staging_size & ~(size_t)1;
    if (bytes < 2) {
        return false;
    }

    int index = audio_free_wavebuf(audio);
    if (index < 0) {
        return false;
    }

    memcpy(audio->pcm[index], audio->pcm_staging, bytes);
    audio->pcm_staging_size = 0;
    audio->wavebufs[index].data_pcm16 = audio->pcm[index];
    audio->wavebufs[index].nsamples = (u32)(bytes / sizeof(s16));
    DSP_FlushDataCache(audio->pcm[index], bytes);
    ndspChnWaveBufAdd(0, &audio->wavebufs[index]);
    audio->buffers_submitted++;
    return true;
}

static void audio_protect_queue(AudioPlayer *audio)
{
    if (!audio_can_control(audio)) {
        return;
    }
    if (audio_queued_wavebufs(audio) <= AUDIO_LOW_WAVEBUFS) {
        audio_submit_partial_staging(audio, AUDIO_PCM_BUFFER_BYTES / 2);
    }
}

static void audio_queue_pcm16_mono_raw(AudioPlayer *audio, const u8 *data, size_t size)
{
    size &= ~(size_t)1;
    while (size > 0) {
        size_t space = AUDIO_PCM_BUFFER_BYTES - audio->pcm_staging_size;
        size_t take = size < space ? size : space;
        memcpy(audio->pcm_staging + audio->pcm_staging_size, data, take);
        audio->pcm_staging_size += take;
        data += take;
        size -= take;
        if (audio->pcm_staging_size >= AUDIO_PCM_BUFFER_BYTES) {
            audio_submit_staging(audio);
        }
    }
}

static void audio_queue_pcm16_mono_scaled(AudioPlayer *audio, const u8 *data, size_t size)
{
    if (!audio || !audio->ndsp_open || audio->failed || audio->muted || !data) {
        return;
    }

    size &= ~(size_t)1;
    int volume = clamp_int(audio->volume_percent, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    while (size > 0) {
        size_t space = AUDIO_PCM_BUFFER_BYTES - audio->pcm_staging_size;
        if (space < 2) {
            if (!audio_submit_staging(audio)) {
                return;
            }
            space = AUDIO_PCM_BUFFER_BYTES - audio->pcm_staging_size;
        }

        size_t take = size < space ? size : space;
        take &= ~(size_t)1;
        for (size_t i = 0; i < take; i += 2) {
            int sample = (s16)((u16)data[i] | ((u16)data[i + 1] << 8));
            int boosted = (sample * volume) / 100;
            s16 out = volume > 100 ? audio_limit_sample(boosted) : clamp_s16(boosted);
            audio->pcm_staging[audio->pcm_staging_size++] = (u8)(out & 0xFF);
            audio->pcm_staging[audio->pcm_staging_size++] = (u8)((out >> 8) & 0xFF);
        }
        data += take;
        size -= take;
        if (audio->pcm_staging_size >= AUDIO_PCM_BUFFER_BYTES) {
            audio_submit_staging(audio);
        }
    }
}

static void audio_queue_sample(AudioPlayer *audio, s16 sample)
{
    if (!audio || !audio->ndsp_open || audio->failed || audio->muted) {
        return;
    }

    int volume = clamp_int(audio->volume_percent, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    int boosted = ((int)sample * volume) / 100;
    s16 out = volume > 100 ? audio_limit_sample(boosted) : clamp_s16(boosted);

    audio->pcm_staging[audio->pcm_staging_size++] = (u8)(out & 0xFF);
    audio->pcm_staging[audio->pcm_staging_size++] = (u8)((out >> 8) & 0xFF);
    if (audio->pcm_staging_size >= AUDIO_PCM_BUFFER_BYTES) {
        audio_submit_staging(audio);
    }
}

static void audio_queue_pcm(AudioPlayer *audio, const u8 *data, size_t size)
{
    if (!audio || !audio->ndsp_open || audio->failed || audio->muted || !data || size == 0) {
        return;
    }

    audio->byte_count += (u32)size;

    int channels = audio->channels > 0 ? audio->channels : AUDIO_CHANNELS;
    int bits = audio->bits_per_sample > 0 ? audio->bits_per_sample : 16;
    if (channels < 1 || channels > 2) {
        channels = 1;
    }

    if (bits == 16 && channels == 1) {
        if (audio->volume_percent == 100) {
            audio_queue_pcm16_mono_raw(audio, data, size);
        } else {
            audio_queue_pcm16_mono_scaled(audio, data, size);
        }
        return;
    }

    if (bits == 16) {
        size_t frame_bytes = (size_t)channels * 2;
        size_t frames = size / frame_bytes;
        for (size_t f = 0; f < frames; f++) {
            int mix = 0;
            for (int ch = 0; ch < channels; ch++) {
                const u8 *p = data + f * frame_bytes + (size_t)ch * 2;
                mix += (s16)((u16)p[0] | ((u16)p[1] << 8));
            }
            audio_queue_sample(audio, (s16)(mix / channels));
        }
    } else if (bits == 8) {
        size_t frame_bytes = (size_t)channels;
        size_t frames = size / frame_bytes;
        for (size_t f = 0; f < frames; f++) {
            int mix = 0;
            for (int ch = 0; ch < channels; ch++) {
                mix += (int)data[f * frame_bytes + (size_t)ch] - 128;
            }
            audio_queue_sample(audio, (s16)((mix / channels) << 8));
        }
    } else {
        audio->overruns++;
    }
}

static bool audio_refill(AudioPlayer *audio)
{
    (void)audio;
    return false;
}

static bool process_mjpeg_frame(MjpegPlayer *player, const u8 *jpeg, size_t len);

static void audio_stop(AudioPlayer *audio)
{
    if (!audio) {
        return;
    }
    if (audio->ndsp_open) {
        ndspChnReset(0);
        ndspExit();
        audio->ndsp_open = false;
    }
    if (audio->context_open) {
        httpcCancelConnection(&audio->context);
        httpcCloseContext(&audio->context);
        audio->context_open = false;
    }
    if (audio->chunk) {
        free(audio->chunk);
        audio->chunk = NULL;
    }
    for (int i = 0; i < AUDIO_WAVEBUF_COUNT; i++) {
        if (audio->pcm[i]) {
            linearFree(audio->pcm[i]);
            audio->pcm[i] = NULL;
        }
    }
}

static bool audio_start(AudioPlayer *audio, const char *url)
{
    (void)url;
    memset(audio, 0, sizeof(*audio));
    audio->http_status = HTTP_STATUS_NONE;
    audio->last_result = 0;
    audio->format_tag = 1;
    audio->channels = AUDIO_CHANNELS;
    audio->bits_per_sample = 16;
    audio->sample_rate = AUDIO_SAMPLE_RATE;
    audio->volume_percent = clamp_int(g_cfg.volume_percent, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    audio->muted = audio->volume_percent == 0;

    Result ret = ndspInit();
    audio->last_result = ret;
    if (R_FAILED(ret)) {
        audio->failed = true;
        return false;
    }
    audio->ndsp_open = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetClippingMode(NDSP_CLIP_SOFT);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    const size_t pcm_bytes = AUDIO_PCM_BUFFER_BYTES;
    for (int i = 0; i < AUDIO_WAVEBUF_COUNT; i++) {
        audio->pcm[i] = (s16 *)linearAlloc(pcm_bytes);
        audio->wavebufs[i].status = NDSP_WBUF_DONE;
    }
    for (int i = 0; i < AUDIO_WAVEBUF_COUNT; i++) {
        if (!audio->pcm[i]) {
            audio->failed = true;
            audio_stop(audio);
            return false;
        }
    }
    return true;
}

static u64 monotonic_ns(void)
{
    return osGetTime() * 1000000ULL;
}

static void mjpeg_pace_frame(MjpegPlayer *player)
{
    if (!player || player->frame_interval_ns == 0) {
        return;
    }

    u64 now = monotonic_ns();
    if (player->next_frame_time_ns == 0) {
        player->next_frame_time_ns = now;
    }

    while (now < player->next_frame_time_ns) {
        if (player->avi_mode && audio_can_control(player->audio) &&
            audio_queued_wavebufs(player->audio) <= AUDIO_LOW_WAVEBUFS) {
            break;
        }
        u64 wait = player->next_frame_time_ns - now;
        if (wait > 10000000ULL) {
            wait = 10000000ULL;
        }
        audio_refill(player->audio);
        if (app_should_exit()) {
            return;
        }
        svcSleepThread(wait);
        now = monotonic_ns();
    }

    player->next_frame_time_ns += player->frame_interval_ns;
    now = monotonic_ns();
    if (player->next_frame_time_ns + player->frame_interval_ns < now) {
        player->next_frame_time_ns = now + player->frame_interval_ns;
    }
}

static bool find_bytes(const u8 *buf, size_t len, size_t from, u8 a, u8 b, size_t *pos)
{
    if (len < 2 || from >= len) {
        return false;
    }
    for (size_t i = from; i + 1 < len; i++) {
        if (buf[i] == a && buf[i + 1] == b) {
            *pos = i;
            return true;
        }
    }
    return false;
}

static u32 read_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 read_le16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

static bool avi_stream_chunk(const u8 *p, char kind0, char kind1)
{
    return isdigit((unsigned char)p[0]) &&
           isdigit((unsigned char)p[1]) &&
           p[2] == kind0 &&
           p[3] == kind1;
}

static void audio_apply_avi_format(AudioPlayer *audio, const u8 *fmt, size_t size)
{
    if (!audio || !fmt || size < 16) {
        return;
    }

    u16 format = read_le16(fmt);
    u16 channels = read_le16(fmt + 2);
    u32 sample_rate = read_le32(fmt + 4);
    u16 bits = read_le16(fmt + 14);

    if (format != 1 || channels < 1 || channels > 2 || sample_rate < 4000 || sample_rate > 48000 || (bits != 8 && bits != 16)) {
        return;
    }

    audio->format_tag = format;
    audio->channels = channels;
    audio->bits_per_sample = bits;
    audio->sample_rate = sample_rate;
    audio->format_known = true;
    if (audio->ndsp_open) {
        ndspChnSetRate(0, (float)sample_rate);
    }
}

static void parse_avi_headers(MjpegPlayer *player, const u8 *buf, size_t len)
{
    if (!player || !player->audio || !buf || len < 8) {
        return;
    }

    bool next_strf_is_audio = false;
    size_t pos = 0;
    while (pos + 8 <= len) {
        const u8 *chunk = buf + pos;
        u32 chunk_size = read_le32(chunk + 4);
        size_t payload = pos + 8;
        size_t next = payload + (size_t)chunk_size + (chunk_size & 1U);
        if (payload > len || next > len) {
            pos++;
            continue;
        }

        if (memcmp(chunk, "strh", 4) == 0) {
            next_strf_is_audio = chunk_size >= 4 && memcmp(buf + payload, "auds", 4) == 0;
            pos = next;
            continue;
        }

        if (memcmp(chunk, "strf", 4) == 0) {
            if (next_strf_is_audio) {
                audio_apply_avi_format(player->audio, buf + payload, chunk_size);
            }
            next_strf_is_audio = false;
            pos = next;
            continue;
        }

        pos++;
    }
}

static bool mjpeg_skip_late_frame(MjpegPlayer *player)
{
    if (!player || player->paused ||
        player->frame_count < 6 || player->next_frame_time_ns == 0 || player->frame_interval_ns == 0) {
        return false;
    }

    u64 now = monotonic_ns();
    u64 late_by = now > player->next_frame_time_ns ? now - player->next_frame_time_ns : 0;
    bool skip = false;
    if (player->avi_mode && audio_can_control(player->audio)) {
        int queued = audio_queued_wavebufs(player->audio);
        skip = (queued <= AUDIO_LOW_WAVEBUFS && late_by >= player->frame_interval_ns) ||
               (late_by >= player->frame_interval_ns * 3ULL);
    } else {
        skip = late_by >= player->frame_interval_ns * 4ULL;
    }
    if (!skip) {
        return false;
    }

    player->next_frame_time_ns += player->frame_interval_ns;
    if (player->next_frame_time_ns + player->frame_interval_ns < now) {
        player->next_frame_time_ns = now + player->frame_interval_ns;
    }
    player->skipped_frame_count++;
    return true;
}

static bool process_avi_video_chunk(MjpegPlayer *player, const u8 *data, size_t size)
{
    if (mjpeg_skip_late_frame(player)) {
        return true;
    }

    if (size >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        return process_mjpeg_frame(player, data, size);
    }

    size_t soi = 0;
    size_t eoi = 0;
    if (find_bytes(data, size, 0, 0xFF, 0xD8, &soi) &&
        find_bytes(data, size, soi + 2, 0xFF, 0xD9, &eoi)) {
        return process_mjpeg_frame(player, data + soi, eoi + 2 - soi);
    }

    player->decode_fail_count++;
    player->last_jpeg_status = PJPG_NOT_JPEG;
    return true;
}

static size_t avi_prequeue_audio_after(MjpegPlayer *player, size_t pos)
{
    if (!player || !player->audio) {
        return pos;
    }

    while (pos + 8 <= player->size) {
        const u8 *chunk = player->buf + pos;
        if (memcmp(chunk, "LIST", 4) == 0) {
            if (pos + 12 > player->size) {
                break;
            }
            if (memcmp(chunk + 8, "rec ", 4) == 0 || memcmp(chunk + 8, "movi", 4) == 0) {
                pos += 12;
                continue;
            }
            break;
        }

        if (!avi_stream_chunk(chunk, 'w', 'b')) {
            break;
        }

        u32 chunk_size = read_le32(chunk + 4);
        if (chunk_size > MJPEG_FRAME_CAP - 16) {
            break;
        }
        size_t total = 8 + (size_t)chunk_size + (chunk_size & 1U);
        if (total > player->size - pos) {
            break;
        }

        audio_queue_pcm(player->audio, chunk + 8, chunk_size);
        audio_refill(player->audio);
        audio_protect_queue(player->audio);
        pos += total;
        if (audio_queued_wavebufs(player->audio) >= AUDIO_TARGET_QUEUED_WAVEBUFS) {
            break;
        }
    }

    return pos;
}

static bool feed_avi_mjpeg_bytes(MjpegPlayer *player, const u8 *data, size_t size)
{
    player->byte_count += (u32)size;

    if (size > MJPEG_FRAME_CAP - player->size) {
        player->size = 0;
        player->avi_in_movi = false;
        set_play_status("AVI buffer overflow; resyncing stream.");
        return true;
    }

    memcpy(player->buf + player->size, data, size);
    player->size += size;

    if (!player->avi_in_movi) {
        size_t movi = 0;
        bool found = false;
        for (size_t i = 0; i + 4 <= player->size; i++) {
            if (memcmp(player->buf + i, "movi", 4) == 0) {
                movi = i + 4;
                found = true;
                break;
            }
        }
        if (!found) {
            size_t keep = player->size < 3 ? player->size : 3;
            if (keep) {
                memmove(player->buf, player->buf + player->size - keep, keep);
            }
            player->size = keep;
            return true;
        }
        parse_avi_headers(player, player->buf, movi);
        memmove(player->buf, player->buf + movi, player->size - movi);
        player->size -= movi;
        player->avi_in_movi = true;
    }

    size_t pos = 0;
    for (;;) {
        size_t remaining = player->size - pos;
        if (remaining < 8) {
            break;
        }

        const u8 *chunk = player->buf + pos;
        if (memcmp(chunk, "LIST", 4) == 0) {
            if (remaining < 12) {
                break;
            }
            u32 list_size = read_le32(chunk + 4);
            if (memcmp(chunk + 8, "rec ", 4) == 0 || memcmp(chunk + 8, "movi", 4) == 0) {
                pos += 12;
                continue;
            }
            size_t total = 8 + (size_t)list_size + (list_size & 1U);
            if (total > remaining) {
                break;
            }
            pos += total;
            continue;
        }

        bool is_video = avi_stream_chunk(chunk, 'd', 'c') || avi_stream_chunk(chunk, 'd', 'b');
        bool is_audio = avi_stream_chunk(chunk, 'w', 'b');
        if (!is_video && !is_audio) {
            size_t next = pos + 1;
            while (next + 8 <= player->size &&
                   memcmp(player->buf + next, "LIST", 4) != 0 &&
                   !avi_stream_chunk(player->buf + next, 'd', 'c') &&
                   !avi_stream_chunk(player->buf + next, 'd', 'b') &&
                   !avi_stream_chunk(player->buf + next, 'w', 'b')) {
                next++;
            }
            if (next + 8 > player->size) {
                size_t keep_from = player->size > 7 ? player->size - 7 : pos;
                if (keep_from > pos) {
                    pos = keep_from;
                }
                break;
            }
            pos = next;
            continue;
        }

        u32 chunk_size = read_le32(chunk + 4);
        if (chunk_size > MJPEG_FRAME_CAP - 16) {
            player->size = 0;
            set_play_status("AVI chunk too large; resyncing.");
            return true;
        }
        size_t total = 8 + (size_t)chunk_size + (chunk_size & 1U);
        if (total > remaining) {
            break;
        }

        const u8 *payload = chunk + 8;
        if (is_video) {
            size_t next_pos = avi_prequeue_audio_after(player, pos + total);
            if (!process_avi_video_chunk(player, payload, chunk_size)) {
                return false;
            }
            pos = next_pos;
        } else {
            audio_queue_pcm(player->audio, payload, chunk_size);
            audio_refill(player->audio);
            audio_protect_queue(player->audio);
            pos += total;
        }
    }

    if (pos > 0) {
        memmove(player->buf, player->buf + pos, player->size - pos);
        player->size -= pos;
    }
    return true;
}

static bool process_mjpeg_frame(MjpegPlayer *player, const u8 *jpeg, size_t len)
{
    int w = 0;
    int h = 0;
    unsigned status = 0;
    if (!decode_jpeg_rgb565(jpeg, len, player->pixels, &w, &h, &status)) {
        player->last_jpeg_status = status;
        player->decode_fail_count++;
        if (player->frame_count == 0 && player->decode_fail_count >= 12) {
            set_play_status("MJPEG JPEG decode failed: %u.", status);
            return false;
        }
        if ((player->decode_fail_count & 0x07) == 0) {
            set_play_status("Skipped unsupported MJPEG frame: %u.", status);
        }
        return true;
    }

    player->last_jpeg_status = 0;
    player->decode_fail_count = 0;
    player->last_width = w;
    player->last_height = h;
    mjpeg_pace_frame(player);
    draw_rgb565_frame(player, player->pixels, w, h);
    audio_refill(player->audio);
    player->frame_count++;
    return true;
}

static bool feed_mjpeg_bytes(MjpegPlayer *player, const u8 *data, size_t size)
{
    player->byte_count += (u32)size;

    if (size > MJPEG_FRAME_CAP - player->size) {
        player->size = 0;
        set_play_status("MJPEG buffer overflow; dropping pending bytes.");
        return true;
    }

    memcpy(player->buf + player->size, data, size);
    player->size += size;

    for (;;) {
        size_t soi = 0;
        if (!find_bytes(player->buf, player->size, 0, 0xFF, 0xD8, &soi)) {
            size_t keep = player->size < 1 ? player->size : 1;
            if (keep) {
                player->buf[0] = player->buf[player->size - 1];
            }
            player->size = keep;
            return true;
        }
        if (soi > 0) {
            memmove(player->buf, player->buf + soi, player->size - soi);
            player->size -= soi;
        }

        size_t eoi = 0;
        if (!find_bytes(player->buf, player->size, 2, 0xFF, 0xD9, &eoi)) {
            return true;
        }

        size_t frame_len = eoi + 2;
        if (!mjpeg_skip_late_frame(player) && !process_mjpeg_frame(player, player->buf, frame_len)) {
            return false;
        }

        memmove(player->buf, player->buf + frame_len, player->size - frame_len);
        player->size -= frame_len;
    }
}

static void mjpeg_free(MjpegPlayer *player)
{
    if (player->buf) {
        free(player->buf);
    }
    if (player->pixels) {
        linearFree(player->pixels);
    }
}

static bool mjpeg_open_retryable(Result ret, u32 status)
{
    if (status == 401 || status == 403) {
        return false;
    }
    if (ret == (Result)HTTPC_RESULTCODE_TIMEDOUT || ret == (Result)0xD9000002) {
        return true;
    }
    if (status == HTTP_STATUS_NONE || status == 0 || status == 404 || status >= 500) {
        return true;
    }
    return R_FAILED(ret);
}

static void set_mjpeg_open_failure_status(const char *prefix, Result ret, u32 status)
{
    if (status == HTTP_STATUS_NONE || status == 0) {
        set_play_status("%s: no HTTP status result 0x%08lX.", prefix, (unsigned long)ret);
    } else {
        set_play_status("%s: HTTP %lu result 0x%08lX.", prefix, (unsigned long)status, (unsigned long)ret);
    }
}

static bool open_mjpeg_stream_with_retries(httpcContext *context, char *url, size_t urlsz, bool avi_container, u64 start_time_ticks, const MjpegPlayer *player, Result *ret_out, u32 *status_out)
{
    Result ret = 0;
    u32 status = HTTP_STATUS_NONE;

    for (int attempt = 0; attempt <= MJPEG_STREAM_OPEN_RETRIES; attempt++) {
        ret = open_stream_context(context, url, &status);
        if (R_SUCCEEDED(ret)) {
            if (ret_out) {
                *ret_out = ret;
            }
            if (status_out) {
                *status_out = status;
            }
            return true;
        }

        if (attempt >= MJPEG_STREAM_OPEN_RETRIES || !mjpeg_open_retryable(ret, status) || app_system_closing()) {
            break;
        }

        set_mjpeg_open_failure_status("MJPEG open retry", ret, status);
        if (!app_system_closing()) {
            mjpeg_console(player, g_play_status);
        }

        stop_active_encoding();
        if (app_wait_or_exit(MJPEG_STREAM_RETRY_SLEEP_NS * (u64)(attempt + 1))) {
            break;
        }
        if (!request_playback_info(start_time_ticks)) {
            break;
        }
        g_stream_switch_serial++;
        build_mjpeg_stream_url(&g_current, url, urlsz, avi_container, start_time_ticks);
        copy_safe(g_play_url, sizeof(g_play_url), url);
    }

    if (ret_out) {
        *ret_out = ret;
    }
    if (status_out) {
        *status_out = status;
    }
    return false;
}

static int stream_reader_priority(void)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio -= 1;
    return clamp_int(prio, 0x18, 0x3F);
}

static void stream_reader_thread(void *arg)
{
    StreamReader *reader = (StreamReader *)arg;
    if (!reader || !reader->context) {
        threadExit(1);
    }

    for (;;) {
        LightLock_Lock(&reader->lock);
        while (!reader->stop && reader->count >= STREAM_READER_BUFFER_COUNT) {
            CondVar_Wait(&reader->can_write, &reader->lock);
        }
        if (reader->stop) {
            reader->finished = true;
            CondVar_Broadcast(&reader->can_read);
            LightLock_Unlock(&reader->lock);
            break;
        }
        int slot = reader->write_index;
        LightLock_Unlock(&reader->lock);

        u32 read_size = 0;
        Result ret = stream_receive_chunk(reader->context, reader->buffers[slot], MJPEG_READ_SIZE, &read_size);

        if (read_size > 0) {
            LightLock_Lock(&reader->lock);
            if (!reader->stop && reader->count < STREAM_READER_BUFFER_COUNT) {
                reader->sizes[slot] = read_size;
                reader->write_index = (reader->write_index + 1) % STREAM_READER_BUFFER_COUNT;
                reader->count++;
                CondVar_Signal(&reader->can_read);
            }
            LightLock_Unlock(&reader->lock);
        }

        if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING || ret == (s32)HTTPC_RESULTCODE_TIMEDOUT) {
            continue;
        }

        if (R_FAILED(ret) || read_size == 0) {
            LightLock_Lock(&reader->lock);
            reader->terminal_result = ret;
            reader->finished = true;
            CondVar_Broadcast(&reader->can_read);
            LightLock_Unlock(&reader->lock);
            break;
        }
    }

    threadExit(0);
}

static void stream_reader_free_buffers(StreamReader *reader)
{
    if (!reader) {
        return;
    }
    bool had_storage = reader->storage != NULL;
    if (reader->storage) {
        free(reader->storage);
        reader->storage = NULL;
    }
    for (int i = 0; i < STREAM_READER_BUFFER_COUNT; i++) {
        if (reader->buffers[i] && !had_storage) {
            free(reader->buffers[i]);
        }
        reader->buffers[i] = NULL;
    }
}

static bool stream_reader_start_on_core(StreamReader *reader, int core_id)
{
    reader->thread = threadCreate(stream_reader_thread, reader,
                                  STREAM_READER_STACK_SIZE,
                                  stream_reader_priority(), core_id, false);
    if (!reader->thread) {
        return false;
    }
    reader->started = true;
    return true;
}

static bool stream_reader_start(StreamReader *reader, httpcContext *context)
{
    if (!reader || !context) {
        return false;
    }

    memset(reader, 0, sizeof(*reader));
    reader->context = context;
    reader->terminal_result = (s32)HTTPC_RESULTCODE_DOWNLOADPENDING;
    LightLock_Init(&reader->lock);
    CondVar_Init(&reader->can_read);
    CondVar_Init(&reader->can_write);

    size_t storage_size = (size_t)STREAM_READER_BUFFER_COUNT * MJPEG_READ_SIZE;
    reader->storage = (u8 *)malloc(storage_size);
    if (!reader->storage) {
        stream_reader_free_buffers(reader);
        return false;
    }
    for (int i = 0; i < STREAM_READER_BUFFER_COUNT; i++) {
        reader->buffers[i] = reader->storage + (size_t)i * MJPEG_READ_SIZE;
    }

    if (g_core1_reader_available && stream_reader_start_on_core(reader, 1)) {
        return true;
    }
    if (stream_reader_start_on_core(reader, -2)) {
        return true;
    }

    stream_reader_free_buffers(reader);
    return false;
}

static Result stream_reader_pop(StreamReader *reader, u8 *out, u32 out_cap, u32 *read_size)
{
    if (read_size) {
        *read_size = 0;
    }
    if (!reader || !out || out_cap == 0) {
        return (s32)HTTPC_RESULTCODE_TIMEDOUT;
    }

    LightLock_Lock(&reader->lock);
    if (reader->count <= 0) {
        Result ret = reader->finished ? reader->terminal_result : (s32)HTTPC_RESULTCODE_TIMEDOUT;
        LightLock_Unlock(&reader->lock);
        return ret;
    }

    int slot = reader->read_index;
    u32 size = reader->sizes[slot];
    if (size > out_cap) {
        size = out_cap;
    }
    memcpy(out, reader->buffers[slot], size);
    reader->read_index = (reader->read_index + 1) % STREAM_READER_BUFFER_COUNT;
    reader->count--;
    CondVar_Signal(&reader->can_write);
    LightLock_Unlock(&reader->lock);

    if (read_size) {
        *read_size = size;
    }
    return (s32)HTTPC_RESULTCODE_DOWNLOADPENDING;
}

static int stream_reader_buffered_chunks(StreamReader *reader)
{
    if (!reader || !reader->started) {
        return 0;
    }

    LightLock_Lock(&reader->lock);
    int count = reader->count;
    LightLock_Unlock(&reader->lock);
    return count;
}

static void stream_reader_prebuffer(StreamReader *reader)
{
    if (!reader || !reader->started) {
        return;
    }

    u64 start_ms = osGetTime();
    while (app_keep_running() &&
           stream_reader_buffered_chunks(reader) < STREAM_READER_PREBUFFER_CHUNKS &&
           osGetTime() - start_ms < STREAM_READER_PREBUFFER_MS) {
        svcSleepThread(5000000ULL);
    }
}

static void stream_reader_stop(StreamReader *reader)
{
    if (!reader) {
        return;
    }

    if (reader->started) {
        LightLock_Lock(&reader->lock);
        reader->stop = true;
        CondVar_Broadcast(&reader->can_read);
        CondVar_Broadcast(&reader->can_write);
        LightLock_Unlock(&reader->lock);

        threadJoin(reader->thread, U64_MAX);
        threadFree(reader->thread);
        reader->thread = NULL;
        reader->started = false;
    }
    stream_reader_free_buffers(reader);
}

static u16 mpeg1_ycbcr_to_rgb565(int y, int cb, int cr)
{
    int c = y - 16;
    int d = cb - 128;
    int e = cr - 128;
    if (c < 0) {
        c = 0;
    }

    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    r = clamp_int(r, 0, 255);
    g = clamp_int(g, 0, 255);
    b = clamp_int(b, 0, 255);
    return rgb565_from_rgb((u8)r, (u8)g, (u8)b);
}

static void draw_mpeg1_frame_rgb565(MjpegPlayer *player, const plm_frame_t *frame)
{
    if (!frame || !frame->y.data || !frame->cb.data || !frame->cr.data ||
        frame->width == 0 || frame->height == 0) {
        return;
    }

    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) {
        return;
    }

    int img_w = (int)frame->width;
    int img_h = (int)frame->height;
    int dst_w = 400;
    int dst_h = (img_h * dst_w) / img_w;
    if (dst_h > 240) {
        dst_h = 240;
        dst_w = (img_w * dst_h) / img_h;
    }
    if (dst_w < 1) {
        dst_w = 1;
    }
    if (dst_h < 1) {
        dst_h = 1;
    }

    int x0 = (400 - dst_w) / 2;
    int y0 = (240 - dst_h) / 2;
    if (dst_w < 400 || dst_h < 240) {
        clear_top_rgb565(0);
    }

    mjpeg_prepare_scale_maps(player, img_w, img_h, dst_w, dst_h);
    const u16 *x_map = player ? player->scale_x : NULL;
    const u16 *y_map = player ? player->scale_y : NULL;

    for (int y = 0; y < dst_h; y++) {
        int sy = y_map ? y_map[y] : (y * img_h) / dst_h;
        if (sy >= img_h) {
            sy = img_h - 1;
        }
        int cy = sy >> 1;
        if (cy >= (int)frame->cb.height) {
            cy = (int)frame->cb.height - 1;
        }
        const u8 *y_row = frame->y.data + (size_t)sy * frame->y.width;
        const u8 *cb_row = frame->cb.data + (size_t)cy * frame->cb.width;
        const u8 *cr_row = frame->cr.data + (size_t)cy * frame->cr.width;
        int screen_y = y0 + y;

        for (int x = 0; x < dst_w; x++) {
            int sx = x_map ? x_map[x] : (x * img_w) / dst_w;
            if (sx >= img_w) {
                sx = img_w - 1;
            }
            int cx = sx >> 1;
            if (cx >= (int)frame->cb.width) {
                cx = (int)frame->cb.width - 1;
            }
            int screen_x = x0 + x;
            fb[screen_x * 240 + (239 - screen_y)] =
                mpeg1_ycbcr_to_rgb565(y_row[sx], cb_row[cx], cr_row[cx]);
        }
    }

    if (player) {
        player->last_width = img_w;
        player->last_height = img_h;
    }
    draw_volume_osd(player, fb);
    draw_quality_osd(player, fb);
    draw_seek_osd(fb);
    draw_remote_message_osd(fb);
    flush_top_video_frame(fb);
}

static void audio_queue_plm_samples(AudioPlayer *audio, const plm_samples_t *samples)
{
    if (!audio || !audio->ndsp_open || audio->failed || audio->muted || !samples) {
        return;
    }

    int volume = clamp_int(audio->volume_percent, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
    for (unsigned i = 0; i < samples->count; i++) {
        float left = samples->interleaved[i * 2];
        float right = samples->interleaved[i * 2 + 1];
        float mixed = (left + right) * 0.5f;
        if (mixed > 1.0f) {
            mixed = 1.0f;
        } else if (mixed < -1.0f) {
            mixed = -1.0f;
        }

        int sample = (int)(mixed * 32767.0f);
        int boosted = (sample * volume) / 100;
        s16 out = volume > 100 ? audio_limit_sample(boosted) : clamp_s16(boosted);

        if (audio->pcm_staging_size + 2 > AUDIO_PCM_BUFFER_BYTES) {
            if (!audio_submit_staging(audio)) {
                return;
            }
        }
        audio->pcm_staging[audio->pcm_staging_size++] = (u8)(out & 0xFF);
        audio->pcm_staging[audio->pcm_staging_size++] = (u8)((out >> 8) & 0xFF);
        if (audio->pcm_staging_size >= AUDIO_PCM_BUFFER_BYTES) {
            audio_submit_staging(audio);
        }
    }
}

static bool mpeg1_decode_audio_until_ready(plm_t *plm, AudioPlayer *audio, int max_frames)
{
    if (!plm || !audio_can_control(audio)) {
        return false;
    }

    bool decoded = false;
    for (int i = 0; i < max_frames; i++) {
        if (audio_queued_wavebufs(audio) >= AUDIO_TARGET_QUEUED_WAVEBUFS) {
            break;
        }
        plm_samples_t *samples = plm_decode_audio(plm);
        if (!samples) {
            break;
        }
        audio_queue_plm_samples(audio, samples);
        audio_refill(audio);
        audio_protect_queue(audio);
        decoded = true;
    }
    return decoded;
}

static void mpeg1_apply_audio_rate(plm_t *plm, AudioPlayer *audio)
{
    if (!plm || !audio || !audio->ndsp_open) {
        return;
    }

    int rate = plm_get_samplerate(plm);
    if (rate >= 8000 && rate <= 48000) {
        audio->sample_rate = (u32)rate;
        ndspChnSetRate(0, (float)rate);
    }
}

static void mpeg1_seek_restart(MjpegPlayer *player, int seconds)
{
    u64 current = mjpeg_current_ticks(player);
    u64 delta = (u64)(seconds < 0 ? -seconds : seconds) * TICKS_PER_SECOND;
    if (seconds < 0) {
        g_mjpeg_resume_ticks = current > delta ? current - delta : 0;
    } else {
        g_mjpeg_resume_ticks = clamp_media_ticks(current + delta);
    }
    g_seek_osd_target_ticks = g_mjpeg_resume_ticks;
    g_seek_osd_delta_seconds = seconds;
    g_seek_osd_pending = true;
    seek_show_osd();
}

static MjpegPlayResult play_mpeg1_stream_url(const char *url, u64 start_time_ticks)
{
    if (!url || !url[0]) {
        set_play_status("No MPEG-1 playback URL.");
        return MJPEG_PLAY_FAILED;
    }

    bool reuse_graphics = g_playback_restart_in_progress;
    if (reuse_graphics) {
        g_playback_restart_in_progress = false;
    } else {
        ui_graphics_exit();
        gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false);
        clear_top_rgb565(0);
        gfxFlushBuffers();
        gfxSwapBuffersGpu();
    }

    MjpegPlayer player;
    memset(&player, 0, sizeof(player));
    player.target_fps = (u32)mjpeg_target_fps();
    player.target_bitrate = (u32)quality_profile().video_bitrate;
    player.start_time_ticks = clamp_media_ticks(start_time_ticks);
    player.position_ticks = player.start_time_ticks;
    player.frame_interval_ns = 1000000000ULL / (player.target_fps ? player.target_fps : 1);
    player.avi_mode = false;

    mjpeg_console(&player, "Opening Jellyfin MPEG-1 stream...");

    httpcContext context;
    char active_url[STREAM_URL_CAP];
    copy_safe(active_url, sizeof(active_url), url);
    u32 status = HTTP_STATUS_NONE;
    Result ret = open_stream_context(&context, active_url, &status);
    if (R_FAILED(ret)) {
        set_mjpeg_open_failure_status("MPEG-1 stream failed", ret, status);
        mjpeg_console(&player, g_play_status);
        app_wait_or_exit(1500000000ULL);
        playback_graphics_exit();
        return MJPEG_PLAY_FAILED;
    }

    AudioPlayer audio;
    memset(&audio, 0, sizeof(audio));
    player.audio = &audio;
    if (!audio_start(&audio, NULL)) {
        set_play_status("MPEG-1 audio init failed; video only.");
    } else {
        audio_show_volume_osd(&audio);
    }

    StreamReader reader;
    bool reader_active = stream_reader_start(&reader, &context);
    if (reader_active) {
        set_play_status("Buffering stream...");
        mjpeg_console(&player, g_play_status);
        stream_reader_prebuffer(&reader);
    }
    u8 *chunk = (u8 *)malloc(MJPEG_READ_SIZE);
    plm_buffer_t *mpeg_buffer = plm_buffer_create_with_capacity(MPEG1_STREAM_BUFFER_CAP);
    plm_t *plm = mpeg_buffer ? plm_create_with_buffer(mpeg_buffer, true) : NULL;
    MjpegPlayResult result = MJPEG_PLAY_FAILED;
    bool mpeg_end_signaled = false;
    bool headers_ready = false;
    bool remote_started = false;

    if (!chunk || !plm) {
        set_play_status("Not enough memory for MPEG-1 playback.");
    } else {
        plm_set_video_enabled(plm, true);
        plm_set_audio_enabled(plm, true);

        if (remote_control_start()) {
            remote_started = true;
            g_session_capabilities_reported = false;
        }
        report_playback_start(player.position_ticks, player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));

        if (g_quality_osd_pending) {
            quality_show_osd();
            g_quality_osd_pending = false;
        }
        if (g_seek_osd_pending) {
            seek_show_osd();
            g_seek_osd_pending = false;
        }

        set_play_status("Buffering MPEG-1 test stream.");
        player.position_clock_ns = monotonic_ns();
        mjpeg_console(&player, g_play_status);

        bool paused_osd_was_visible = false;
        int no_frame_loops = 0;
        while (app_keep_running()) {
            hidScanInput();
            u32 down = hidKeysDown();
            if (down & KEY_START) {
                g_exit_requested = true;
                set_play_status("Playback stopped.");
                break;
            }
            if (down & KEY_B) {
                set_play_status("Playback stopped.");
                result = MJPEG_PLAY_OK;
                break;
            }
            if (down & KEY_A) {
                mjpeg_set_paused(&player, !player.paused);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (!player.paused) {
                    paused_osd_was_visible = false;
                }
            }
            if (down & KEY_UP) {
                audio_change_volume(&audio, VOLUME_STEP_PERCENT);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (player.paused) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = audio_volume_osd_visible(&audio);
                }
            }
            if (down & KEY_DOWN) {
                audio_change_volume(&audio, -VOLUME_STEP_PERCENT);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (player.paused) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = audio_volume_osd_visible(&audio);
                }
            }
            if (down & KEY_Y) {
                audio_toggle_mute(&audio);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (player.paused) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = audio_volume_osd_visible(&audio);
                }
                mjpeg_console(&player, g_play_status);
            }
            if (down & KEY_L) {
                mpeg1_seek_restart(&player, -SEEK_STEP_SECONDS);
                report_playback_progress(g_mjpeg_resume_ticks, true, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                result = MJPEG_PLAY_RESTART;
                break;
            }
            if (down & KEY_R) {
                mpeg1_seek_restart(&player, SEEK_STEP_SECONDS);
                report_playback_progress(g_mjpeg_resume_ticks, true, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                result = MJPEG_PLAY_RESTART;
                break;
            }
            if (down & KEY_DLEFT) {
                g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
                report_playback_progress(g_mjpeg_resume_ticks, player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                change_quality(-1);
                g_stream_switch_serial++;
                quality_show_osd();
                g_quality_osd_pending = true;
                result = MJPEG_PLAY_RESTART;
                break;
            }
            if (down & KEY_DRIGHT) {
                g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
                report_playback_progress(g_mjpeg_resume_ticks, player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                change_quality(1);
                g_stream_switch_serial++;
                quality_show_osd();
                g_quality_osd_pending = true;
                result = MJPEG_PLAY_RESTART;
                break;
            }

            RemoteCommand remote_cmd;
            if (remote_control_poll(&remote_cmd)) {
                if (mjpeg_apply_remote_command(&player, &audio, &remote_cmd, &result)) {
                    break;
                }
            }

            report_playback_progress_periodic(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));

            u32 read_size = 0;
            ret = reader_active ? stream_reader_pop(&reader, chunk, MJPEG_READ_SIZE, &read_size)
                                : stream_receive_chunk(&context, chunk, MJPEG_READ_SIZE, &read_size);
            if (read_size) {
                plm_buffer_write(mpeg_buffer, chunk, read_size);
            }
            if (!mpeg_end_signaled && ret != (s32)HTTPC_RESULTCODE_DOWNLOADPENDING && ret != (s32)HTTPC_RESULTCODE_TIMEDOUT) {
                if (R_FAILED(ret)) {
                    if (player.frame_count >= MJPEG_STREAM_READ_RECONNECT_FRAMES) {
                        g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
                        g_stream_switch_serial++;
                        set_play_status("MPEG-1 stream interrupted; reconnecting.");
                        result = MJPEG_PLAY_RESTART;
                    } else {
                        set_play_status("MPEG-1 read failed: 0x%08lX.", (unsigned long)ret);
                    }
                    break;
                }
                plm_buffer_signal_end(mpeg_buffer);
                mpeg_end_signaled = true;
            }

            if (!headers_ready) {
                headers_ready = plm_has_headers(plm);
                if (headers_ready) {
                    mpeg1_apply_audio_rate(plm, &audio);
                    set_play_status("Playing MPEG-1 test.");
                    mjpeg_console(&player, g_play_status);
                } else {
                    if (!read_size && reader_active) {
                        svcSleepThread(STREAM_READER_EMPTY_SLEEP_NS);
                    }
                    continue;
                }
            }

            if (player.paused) {
                bool osd_visible = audio_volume_osd_visible(&audio) || seek_osd_visible() || quality_osd_visible();
                if (osd_visible || paused_osd_was_visible) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = osd_visible;
                }
                svcSleepThread(50000000ULL);
                continue;
            }

            mpeg1_decode_audio_until_ready(plm, &audio, 3);
            audio_refill(player.audio);

            plm_frame_t *frame = plm_decode_video(plm);
            if (frame) {
                no_frame_loops = 0;
                mjpeg_pace_frame(&player);
                draw_mpeg1_frame_rgb565(&player, frame);
                player.frame_count++;
                continue;
            }

            if (plm_has_ended(plm)) {
                set_play_status("MPEG-1 reached end of stream.");
                result = MJPEG_PLAY_OK;
                break;
            }

            no_frame_loops++;
            if (!read_size && reader_active) {
                svcSleepThread(STREAM_READER_EMPTY_SLEEP_NS);
            } else if (no_frame_loops > 20) {
                svcSleepThread(5000000ULL);
            }
        }
    }

    if (result != MJPEG_PLAY_RESTART) {
        g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
        report_playback_stopped(g_mjpeg_resume_ticks);
    }
    if (!app_system_closing() && remote_started && result != MJPEG_PLAY_RESTART) {
        remote_control_stop();
    }
    if (g_volume_save_pending && !app_system_closing()) {
        save_config();
        g_volume_save_pending = false;
    }
    if (plm) {
        plm_destroy(plm);
    } else if (mpeg_buffer) {
        plm_buffer_destroy(mpeg_buffer);
    }
    if (reader_active) {
        stream_reader_stop(&reader);
    }
    if (chunk) {
        free(chunk);
    }
    if (!app_system_closing()) {
        audio_stop(&audio);
        httpcCancelConnection(&context);
        httpcCloseContext(&context);
    }
    if (!app_system_closing() && result == MJPEG_PLAY_RESTART) {
        stop_active_encoding();
        svcSleepThread(120000000ULL);
    }
    if (!app_system_closing() && result != MJPEG_PLAY_RESTART) {
        mjpeg_console(&player, g_play_status);
    }
    if (!g_exit_requested && result != MJPEG_PLAY_RESTART) {
        app_wait_or_exit(700000000ULL);
    }
    g_playback_restart_in_progress = result == MJPEG_PLAY_RESTART;
    playback_graphics_exit();
    return result;
}

static MjpegPlayResult play_mjpeg_stream_url(const char *url, bool avi_container, u64 start_time_ticks)
{
    if (!url || !url[0]) {
        set_play_status("No MJPEG playback URL.");
        return MJPEG_PLAY_FAILED;
    }

    bool reuse_graphics = g_playback_restart_in_progress;
    if (reuse_graphics) {
        g_playback_restart_in_progress = false;
    } else {
        ui_graphics_exit();
        gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false);
        clear_top_rgb565(0);
        gfxFlushBuffers();
        gfxSwapBuffersGpu();
    }

    MjpegPlayer player;
    memset(&player, 0, sizeof(player));
    player.target_fps = (u32)mjpeg_target_fps();
    player.target_bitrate = (u32)mjpeg_target_bitrate();
    player.start_time_ticks = clamp_media_ticks(start_time_ticks);
    player.position_ticks = player.start_time_ticks;
    player.frame_interval_ns = 1000000000ULL / (player.target_fps ? player.target_fps : 1);
    player.avi_mode = avi_container;
    player.buf = (u8 *)malloc(MJPEG_FRAME_CAP);
    player.pixels = (u16 *)linearMemAlign(JPEG_PIXELS_CAP * sizeof(u16), 0x40);
    if (!player.buf || !player.pixels) {
        set_play_status("Not enough memory for MJPEG playback.");
        if (!app_system_closing()) {
            report_playback_stopped(player.position_ticks);
        }
        if (!app_system_closing()) {
            mjpeg_console(&player, g_play_status);
        }
        app_wait_or_exit(1500000000ULL);
        mjpeg_free(&player);
        playback_graphics_exit();
        return MJPEG_PLAY_FAILED;
    }

    mjpeg_console(&player, "Opening Jellyfin MJPEG stream...");

    httpcContext context;
    char active_url[STREAM_URL_CAP];
    copy_safe(active_url, sizeof(active_url), url);
    u32 status = HTTP_STATUS_NONE;
    Result ret = 0;
    if (!open_mjpeg_stream_with_retries(&context, active_url, sizeof(active_url), avi_container, start_time_ticks, &player, &ret, &status)) {
        set_mjpeg_open_failure_status("MJPEG stream failed", ret, status);
        if (!app_system_closing()) {
            report_playback_stopped(player.position_ticks);
        }
        if (!app_system_closing()) {
            mjpeg_console(&player, g_play_status);
        }
        app_wait_or_exit(1500000000ULL);
        mjpeg_free(&player);
        playback_graphics_exit();
        return MJPEG_PLAY_FAILED;
    }

    AudioPlayer audio;
    memset(&audio, 0, sizeof(audio));
    player.audio = &audio;
    if (avi_container && !audio_start(&audio, NULL)) {
        set_play_status("Audio init failed; playing video only.");
    } else if (avi_container) {
        audio_show_volume_osd(&audio);
    }
    if (remote_control_start()) {
        g_session_capabilities_reported = false;
    }
    report_playback_start(player.position_ticks, player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));

    u8 *chunk = (u8 *)malloc(MJPEG_READ_SIZE);
    MjpegPlayResult result = MJPEG_PLAY_FAILED;
    if (!chunk) {
        set_play_status("Could not allocate MJPEG read buffer.");
    } else {
        StreamReader reader;
        bool reader_active = stream_reader_start(&reader, &context);
        if (reader_active) {
            set_play_status("Buffering stream...");
            mjpeg_console(&player, g_play_status);
            stream_reader_prebuffer(&reader);
        }
        if (g_quality_osd_pending) {
            quality_show_osd();
            g_quality_osd_pending = false;
        }
        if (g_seek_osd_pending) {
            seek_show_osd();
            g_seek_osd_pending = false;
        }
        set_play_status("Playing.");
        player.position_clock_ns = monotonic_ns();
        mjpeg_console(&player, g_play_status);
        bool paused_osd_was_visible = false;
        bool seek_commit_pending = false;
        u64 seek_base_ticks = 0;
        u64 seek_commit_ready_ms = 0;
        int seek_hold_dir = 0;
        u64 seek_hold_start_ms = 0;
        while (app_keep_running()) {
            hidScanInput();
            u32 down = hidKeysDown();
            u32 held = hidKeysHeld();
            if (down & KEY_START) {
                g_exit_requested = true;
                set_play_status("Playback stopped.");
                break;
            }
            if (down & KEY_B) {
                set_play_status("Playback stopped.");
                result = MJPEG_PLAY_OK;
                break;
            }
            if (down & KEY_A) {
                mjpeg_set_paused(&player, !player.paused);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (!player.paused) {
                    paused_osd_was_visible = false;
                }
            }
            if (down & KEY_UP) {
                audio_change_volume(&audio, VOLUME_STEP_PERCENT);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (player.paused) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = audio_volume_osd_visible(&audio);
                }
            }
            if (down & KEY_DOWN) {
                audio_change_volume(&audio, -VOLUME_STEP_PERCENT);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (player.paused) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = audio_volume_osd_visible(&audio);
                }
            }
            if (down & KEY_Y) {
                audio_toggle_mute(&audio);
                report_playback_progress(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                if (player.paused) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = audio_volume_osd_visible(&audio);
                }
                mjpeg_console(&player, g_play_status);
            }

            RemoteCommand remote_cmd;
            if (remote_control_poll(&remote_cmd)) {
                if (mjpeg_apply_remote_command(&player, &audio, &remote_cmd, &result)) {
                    break;
                }
            }

            int seek_seconds = 0;
            u64 now_ms = osGetTime();
            int seek_dir = 0;
            if ((down & KEY_L) || ((held & KEY_L) && !(held & KEY_R))) {
                seek_dir = -1;
            } else if ((down & KEY_R) || ((held & KEY_R) && !(held & KEY_L))) {
                seek_dir = 1;
            }

            if (seek_dir != 0) {
                if (seek_hold_dir != seek_dir) {
                    seek_hold_dir = seek_dir;
                    seek_hold_start_ms = now_ms;
                    g_seek_repeat_ready_ms = now_ms;
                }
                u64 held_ms = now_ms >= seek_hold_start_ms ? now_ms - seek_hold_start_ms : 0;
                bool pressed_now = (seek_dir < 0 && (down & KEY_L)) || (seek_dir > 0 && (down & KEY_R));
                if (pressed_now || now_ms >= g_seek_repeat_ready_ms) {
                    int step_seconds = seek_step_seconds_for_hold(held_ms);
                    seek_seconds = seek_dir * step_seconds;
                    g_seek_repeat_ready_ms = now_ms + seek_repeat_delay_for_hold(held_ms);
                }
            } else if (!(held & (KEY_L | KEY_R))) {
                seek_hold_dir = 0;
                seek_hold_start_ms = 0;
            }

            if (seek_seconds != 0) {
                if (!seek_commit_pending) {
                    seek_base_ticks = mjpeg_current_ticks(&player);
                    player.position_ticks = seek_base_ticks;
                    player.position_clock_ns = 0;
                    player.paused = true;
                    audio_set_paused(player.audio, true);
                }
                seek_commit_ready_ms = now_ms + SEEK_COMMIT_DELAY_MS;
                bool moved = mjpeg_preview_seek(&player, seek_seconds, seek_base_ticks, seek_commit_pending);
                seek_commit_pending = seek_commit_pending || moved;
                mjpeg_redraw_last_frame(&player);
                mjpeg_console(&player, g_play_status);
            }

            if (seek_commit_pending && !(held & (KEY_L | KEY_R)) && now_ms >= seek_commit_ready_ms) {
                mjpeg_commit_seek();
                mjpeg_redraw_last_frame(&player);
                mjpeg_console(&player, g_play_status);
                report_playback_progress(g_mjpeg_resume_ticks, true, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                result = MJPEG_PLAY_RESTART;
                break;
            }

            if (down & KEY_DLEFT) {
                g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
                report_playback_progress(g_mjpeg_resume_ticks, player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                change_quality(-1);
                g_stream_switch_serial++;
                char quality_label[16];
                format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
                quality_show_osd();
                g_quality_osd_pending = true;
                set_play_status("Switching to %s.", quality_label);
                mjpeg_redraw_last_frame(&player);
                mjpeg_console(&player, g_play_status);
                result = MJPEG_PLAY_RESTART;
                break;
            }
            if (down & KEY_DRIGHT) {
                g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
                report_playback_progress(g_mjpeg_resume_ticks, player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));
                change_quality(1);
                g_stream_switch_serial++;
                char quality_label[16];
                format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
                quality_show_osd();
                g_quality_osd_pending = true;
                set_play_status("Switching to %s.", quality_label);
                mjpeg_redraw_last_frame(&player);
                mjpeg_console(&player, g_play_status);
                result = MJPEG_PLAY_RESTART;
                break;
            }

            report_playback_progress_periodic(mjpeg_current_ticks(&player), player.paused, audio_muted_for_report(&audio), audio_volume_for_report(&audio));

            if (player.paused) {
                bool osd_visible = audio_volume_osd_visible(&audio) || seek_osd_visible() || quality_osd_visible();
                if (osd_visible || paused_osd_was_visible) {
                    mjpeg_redraw_last_frame(&player);
                    paused_osd_was_visible = osd_visible;
                }
                svcSleepThread(50000000ULL);
                continue;
            }

            audio_refill(player.audio);
            u32 read_size = 0;
            if (reader_active) {
                ret = stream_reader_pop(&reader, chunk, MJPEG_READ_SIZE, &read_size);
            } else {
                ret = stream_receive_chunk(&context, chunk, MJPEG_READ_SIZE, &read_size);
            }
            if (read_size) {
                bool fed = avi_container ? feed_avi_mjpeg_bytes(&player, chunk, read_size)
                                         : feed_mjpeg_bytes(&player, chunk, read_size);
                if (!fed) {
                    break;
                }
            }
            if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING || ret == (s32)HTTPC_RESULTCODE_TIMEDOUT) {
                if (!read_size && reader_active) {
                    svcSleepThread(STREAM_READER_EMPTY_SLEEP_NS);
                }
                continue;
            }
            if (R_FAILED(ret)) {
                if (player.frame_count >= MJPEG_STREAM_READ_RECONNECT_FRAMES) {
                    g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
                    g_stream_switch_serial++;
                    set_play_status("MJPEG stream interrupted; reconnecting.");
                    mjpeg_redraw_last_frame(&player);
                    mjpeg_console(&player, g_play_status);
                    result = MJPEG_PLAY_RESTART;
                } else {
                    set_play_status("MJPEG read failed: 0x%08lX.", (unsigned long)ret);
                }
                break;
            }
            set_play_status("MJPEG reached end of stream.");
            result = MJPEG_PLAY_OK;
            break;
        }
        if (reader_active) {
            stream_reader_stop(&reader);
        }
        free(chunk);
    }

    if (result != MJPEG_PLAY_RESTART) {
        g_mjpeg_resume_ticks = mjpeg_current_ticks(&player);
    }

    if (result != MJPEG_PLAY_RESTART) {
        report_playback_stopped(g_mjpeg_resume_ticks);
    }
    if (!app_system_closing() && result != MJPEG_PLAY_RESTART) {
        remote_control_stop();
    }

    if (g_volume_save_pending && !app_system_closing()) {
        save_config();
        g_volume_save_pending = false;
    }

    if (!app_system_closing()) {
        audio_stop(&audio);
        httpcCancelConnection(&context);
        httpcCloseContext(&context);
    }

    if (!app_system_closing() && result == MJPEG_PLAY_RESTART) {
        stop_active_encoding();
        svcSleepThread(120000000ULL);
    }

    if (!app_system_closing() && result != MJPEG_PLAY_RESTART) {
        mjpeg_console(&player, g_play_status);
    }
    if (!g_exit_requested && result != MJPEG_PLAY_RESTART) {
        app_wait_or_exit(700000000ULL);
    }
    if (!app_system_closing()) {
        mjpeg_free(&player);
    }
    g_playback_restart_in_progress = result == MJPEG_PLAY_RESTART;
    playback_graphics_exit();
    return result;
}

static bool play_current_item_video(void)
{
    g_mjpeg_resume_ticks = 0;
    g_seek_osd_until_ms = 0;
    g_seek_osd_target_ticks = 0;
    g_seek_osd_delta_seconds = 0;
    g_seek_repeat_ready_ms = 0;
    g_seek_osd_pending = false;
    g_quality_osd_pending = false;
    g_quality_osd_until_ms = 0;

    bool prefer_h264 = g_cfg.playback_mode == PLAYBACK_MODE_AUTO || g_cfg.playback_mode == PLAYBACK_MODE_H264;
    bool prefer_mpeg1 = false;

    if (g_is_new_3ds && prefer_h264) {
        set_play_status("New3DS detected: using H.264/MVD playback.");
        if (play_stream_url(g_play_url)) {
            return true;
        }
        if (g_exit_requested) {
            return false;
        }
        set_play_status("MVD unavailable; falling back to MJPEG software playback.");
    } else if (g_is_new_3ds) {
        set_play_status("New3DS detected: using selected software playback.");
    } else {
        set_play_status("Old3DS detected: using MJPEG software playback.");
    }

    u64 start_ticks = 0;
    if (prefer_mpeg1) {
        char mpeg_url[STREAM_URL_CAP];
        for (int alias = 0; alias < 2; alias++) {
            build_mpeg1_stream_url(&g_current, mpeg_url, sizeof(mpeg_url), start_ticks, alias != 0);
            copy_safe(g_play_url, sizeof(g_play_url), mpeg_url);
            copy_safe(g_play_method, sizeof(g_play_method), alias ? "mpeg1-mp2-mpg-test" : "mpeg1-mp2-test");
            set_play_status(alias ? "Trying MPEG-1 mpg alias..." : "Trying MPEG-1 test stream...");
            MjpegPlayResult result = play_mpeg1_stream_url(g_play_url, start_ticks);
            if (result == MJPEG_PLAY_RESTART) {
                start_ticks = g_mjpeg_resume_ticks;
                if (!request_playback_info(start_ticks)) {
                    g_playback_restart_in_progress = false;
                    playback_graphics_exit();
                    return false;
                }
                prefer_mpeg1 = false;
                if (!prefer_mpeg1) {
                    break;
                }
                alias = -1;
                continue;
            }
            if (result == MJPEG_PLAY_OK) {
                return true;
            }
            if (g_exit_requested) {
                return false;
            }
        }
        set_play_status("MPEG-1 test failed; falling back to MJPEG.");
    }

    char mjpeg_url[STREAM_URL_CAP];
    for (;;) {
        build_mjpeg_stream_url(&g_current, mjpeg_url, sizeof(mjpeg_url), true, start_ticks);
        copy_safe(g_play_url, sizeof(g_play_url), mjpeg_url);
        copy_safe(g_play_method, sizeof(g_play_method), g_is_new_3ds ? "avi-mjpeg-pcm-fallback" : "old3ds-avi-mjpeg-pcm");
        MjpegPlayResult result = play_mjpeg_stream_url(g_play_url, true, start_ticks);
        if (result == MJPEG_PLAY_RESTART) {
            start_ticks = g_mjpeg_resume_ticks;
            if (!request_playback_info(start_ticks)) {
                g_playback_restart_in_progress = false;
                playback_graphics_exit();
                return false;
            }
            continue;
        }
        if (result == MJPEG_PLAY_OK) {
            return true;
        }
        start_ticks = g_mjpeg_resume_ticks;
        break;
    }
    if (g_exit_requested) {
        return false;
    }

    for (;;) {
        build_mjpeg_stream_url(&g_current, mjpeg_url, sizeof(mjpeg_url), false, start_ticks);
        copy_safe(g_play_url, sizeof(g_play_url), mjpeg_url);
        copy_safe(g_play_method, sizeof(g_play_method), g_is_new_3ds ? "raw-mjpeg-fallback" : "old3ds-raw-mjpeg");
        set_play_status("Trying raw MJPEG fallback...");
        MjpegPlayResult result = play_mjpeg_stream_url(g_play_url, false, start_ticks);
        if (result == MJPEG_PLAY_RESTART) {
            start_ticks = g_mjpeg_resume_ticks;
            if (!request_playback_info(start_ticks)) {
                g_playback_restart_in_progress = false;
                playback_graphics_exit();
                return false;
            }
            continue;
        }
        return result == MJPEG_PLAY_OK;
    }
}

static bool probe_playback(void)
{
    if (!g_current.id[0]) {
        return false;
    }

    g_view = VIEW_PLAYBACK;
    g_play_url[0] = 0;
    g_play_method[0] = 0;
    g_play_media_source_id[0] = 0;
    g_play_status[0] = 0;

    if (!request_playback_info(0)) {
        if (!g_exit_requested) {
            g_view = g_return_view;
        }
        return false;
    }
    play_current_item_video();
    if (!g_exit_requested) {
        g_view = g_return_view;
    }
    return true;
}

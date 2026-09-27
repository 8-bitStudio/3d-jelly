typedef enum { MUSIC_INFO, MUSIC_STREAM, MUSIC_ENDED, MUSIC_ERROR } MusicPhase;
typedef struct {
    bool active, dsp, paused, muted, buffering, started;
    bool http_added, output_ready, volume_dirty;
    MusicSoundSettings sound;
    MusicPhase phase;
    View return_view;
    CURLM *multi;
    CURL *http;
    struct curl_slist *headers;
    CurlResponseBuffer info;
    MusicPcmStream stream;
    ndspWaveBuf wave[MUSIC_BUFFER_COUNT];
    s16 *pcm[MUSIC_BUFFER_COUNT];
    bool queued[MUSIC_BUFFER_COUNT];
    u64 base_ticks, completed_frames;
    u32 audio_rate, underruns;
    u16 audio_channels;
    size_t buffered_bytes;
    int queue[MAX_ITEMS], count, index;
    char body[1024], error[160];
} MusicPlayer;
static MusicPlayer g_music;
static MusicSoundSettings g_music_settings_draft;
static View g_music_settings_return_view;
static int g_music_settings_row;

/* Only the network worker touches the stream curl handles while workers run.
 * Ring/header/control fields are protected by io.lock; only the output worker
 * touches DSP buffers. The UI consumes a small published snapshot. */
typedef struct {
    bool started, buffering, finished;
    u64 position;
    u32 rate, underruns;
    u16 channels;
    size_t buffered_bytes;
} MusicOutputState;
typedef struct {
    LightLock lock;
    LightEvent network_wake, output_wake;
    Thread network_thread, output_thread;
    bool running, stop, net_done, flow_paused, paused, muted;
    int volume;
    u64 last_data_ms;
    MusicOutputState output;
    char error[160];
} MusicIo;
static MusicIo g_music_io;
static bool g_music_io_initialized;
static void music_workers_stop(void);

static u64 music_output_position(void)
{
    u64 frames = g_music.completed_frames;
    if (g_music.dsp) {
        u16 sequence = ndspChnGetWaveBufSeq(0);
        for (int i = 0; i < MUSIC_BUFFER_COUNT; ++i) {
            if (!g_music.queued[i]) continue;
            if (g_music.wave[i].status == NDSP_WBUF_DONE) frames += g_music.wave[i].nsamples;
            else if (g_music.wave[i].sequence_id == sequence && g_music.wave[i].status == NDSP_WBUF_PLAYING) {
                u32 pos = ndspChnGetSamplePos(0);
                frames += pos < g_music.wave[i].nsamples ? pos : g_music.wave[i].nsamples;
            }
        }
    }
    u64 ticks = g_music.base_ticks + (g_music.stream.rate ? frames * TICKS_PER_SECOND / g_music.stream.rate : 0);
    return g_current.runtime_ticks && ticks > g_current.runtime_ticks ? g_current.runtime_ticks : ticks;
}

static u64 music_position(void)
{
    if (!g_music_io.running) return music_output_position();
    LightLock_Lock(&g_music_io.lock);
    u64 position = g_music_io.output.position;
    LightLock_Unlock(&g_music_io.lock);
    return position;
}

static void music_detach_http(void)
{
    if (g_music.http_added) curl_multi_remove_handle(g_music.multi, g_music.http);
    g_music.http_added = false;
    curl_slist_free_all(g_music.headers);
    free(g_music.info.data);
    memset(&g_music.info, 0, sizeof(g_music.info));
    g_music.headers = NULL;
}

static void music_close_http(void)
{
    music_detach_http();
    if (g_music.http) curl_easy_cleanup(g_music.http);
    if (g_music.multi) curl_multi_cleanup(g_music.multi);
    g_music.http = NULL; g_music.multi = NULL;
}

static void music_release(bool ended, bool reuse)
{
    music_workers_stop();
    u64 position = music_position();
    if (reuse) music_detach_http(); else music_close_http();
    if (g_music.dsp) {
        ndspChnReset(0);
        if (!reuse) { ndspExit(); g_music.dsp = false; }
    }
    g_music.output_ready = false;
    for (int i = 0; i < MUSIC_BUFFER_COUNT; ++i) {
        if (!reuse) {
            if (g_music.pcm[i]) linearFree(g_music.pcm[i]);
            g_music.pcm[i] = NULL;
        }
        memset(&g_music.wave[i], 0, sizeof(ndspWaveBuf));
        g_music.queued[i] = false;
    }
    u8 *data = reuse ? g_music.stream.data : NULL;
    size_t capacity = reuse ? g_music.stream.capacity : 0;
    if (!reuse) free(g_music.stream.data);
    memset(&g_music.stream, 0, sizeof(g_music.stream));
    g_music.stream.data = data; g_music.stream.capacity = capacity;
    g_music.completed_frames = 0;
    g_music.base_ticks = position;
    g_playback_ended_naturally = ended;
    if (g_playback_report_active) report_playback_stopped(position);
    if (g_play_session[0] && !app_system_closing()) stop_active_encoding();
    g_play_session[0] = g_play_media_source_id[0] = 0;
}

static void music_stop(void)
{
    if (!g_music.active) return;
    music_release(false, false);
    if (g_music.volume_dirty && !app_system_closing()) save_config();
    g_music.active = false;
}

static void music_fail(const char *message)
{
    copy_safe(g_music.error, sizeof(g_music.error), message);
    music_release(false, false);
    g_music.phase = MUSIC_ERROR;
    g_music.buffering = false;
}

static size_t music_receive(void *data, size_t size, size_t count, void *user)
{
    (void)user;
    size_t bytes = size * count;
    LightLock_Lock(&g_music_io.lock);
    if (g_music_io.stop) { LightLock_Unlock(&g_music_io.lock); return 0; }
    if (!music_ring_write(&g_music.stream, data, bytes)) {
        g_music_io.flow_paused = true;
        LightLock_Unlock(&g_music_io.lock);
        return CURL_WRITEFUNC_PAUSE;
    }
    g_music_io.last_data_ms = osGetTime();
    LightLock_Unlock(&g_music_io.lock);
    LightEvent_Signal(&g_music_io.output_wake);
    return bytes;
}

static bool music_http_begin(const char *path, bool info)
{
    if (!curl_http_runtime_init()) return false;
    char url[STREAM_URL_CAP], auth[384], header[448];
    build_url(url, sizeof(url), path);
    auth_header(auth, sizeof(auth), true);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    if (!g_music.multi) g_music.multi = curl_multi_init();
    if (!g_music.http) g_music.http = curl_easy_init();
    if (!g_music.multi || !g_music.http) { music_close_http(); return false; }
    /* Reset options, retaining curl's DNS/TLS/connection caches across requests. */
    curl_easy_reset(g_music.http);
    g_music.headers = curl_slist_append(NULL, header);
    g_music.headers = curl_slist_append(g_music.headers, "Content-Type: application/json");
    curl_easy_setopt(g_music.http, CURLOPT_URL, url);
    curl_easy_setopt(g_music.http, CURLOPT_HTTPHEADER, g_music.headers);
    curl_easy_setopt(g_music.http, CURLOPT_USERAGENT, "3dJelly/" APP_VERSION);
    curl_easy_setopt(g_music.http, CURLOPT_CONNECTTIMEOUT_MS, 6000L);
    curl_easy_setopt(g_music.http, CURLOPT_TIMEOUT_MS, info ? 15000L : 0L);
    curl_easy_setopt(g_music.http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_music.http, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_music.http, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(g_music.http, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(g_music.http, CURLOPT_SSL_VERIFYHOST, 0L);
    if (info) {
        curl_easy_setopt(g_music.http, CURLOPT_POSTFIELDS, g_music.body);
        curl_easy_setopt(g_music.http, CURLOPT_WRITEFUNCTION, curl_write_response);
        curl_easy_setopt(g_music.http, CURLOPT_WRITEDATA, &g_music.info);
    } else {
        curl_easy_setopt(g_music.http, CURLOPT_WRITEFUNCTION, music_receive);
        curl_easy_setopt(g_music.http, CURLOPT_WRITEDATA, NULL);
    }
    g_music.http_added = curl_multi_add_handle(g_music.multi, g_music.http) == CURLM_OK;
    return g_music.http_added;
}

static void music_open(u64 ticks)
{
    g_music.phase = MUSIC_INFO;
    g_music.base_ticks = ticks;
    g_music.buffering = true; g_music.started = false;
    g_music.audio_rate = g_music.audio_channels = g_music.underruns = 0;
    g_music.buffered_bytes = 0;
    g_music.error[0] = 0;
    g_music.sound = g_cfg.music;
    music_sound_normalize(&g_music.sound);
    g_playback_ended_naturally = false;
    music_build_playback_body(g_music.body, sizeof(g_music.body), ticks, &g_music.sound);
    char id[240], path[400];
    url_encode(g_current.id, id, sizeof(id));
    snprintf(path, sizeof(path), "/Items/%s/PlaybackInfo?UserId=%s", id, g_cfg.user_id);
    if (!music_http_begin(path, true)) music_fail("Could not connect. Press A to retry.");
}

static bool music_start(const MediaItem *item)
{
    if (!music_is_audio(item)) return false;
    MediaItem copy = *item;
    browse_cancel_request();
    music_stop();
    memset(&g_music, 0, sizeof(g_music));
    if (!g_music_io_initialized) {
        LightLock_Init(&g_music_io.lock);
        LightEvent_Init(&g_music_io.network_wake, RESET_ONESHOT);
        LightEvent_Init(&g_music_io.output_wake, RESET_ONESHOT);
        g_music_io_initialized = true;
    }
    g_music.active = true;
    g_music.return_view = g_view;
    g_music.muted = g_cfg.music.volume_percent == 0;
    if (g_view == VIEW_ITEMS || g_view == VIEW_SEARCH) {
        for (int i = 0; i < g_item_count; ++i) if (music_is_audio(&g_items[i]) && is_playable(&g_items[i])) {
            if (!strcmp(g_items[i].id, copy.id)) g_music.index = g_music.count;
            g_music.queue[g_music.count++] = i;
        }
    }
    if (!g_music.count) { g_music.queue[0] = -1; g_music.count = 1; }
    g_current = copy;
    g_play_session[0] = g_play_media_source_id[0] = 0;
    g_view = VIEW_MUSIC;
    music_open(0);
    return true;
}

static void music_apply_volume(void)
{
    if (!g_music_io.running) return;
    LightLock_Lock(&g_music_io.lock);
    g_music_io.volume = g_cfg.music.volume_percent;
    g_music_io.muted = g_music.muted;
    g_music_io.paused = g_music.paused;
    if (!g_music.paused) g_music_io.last_data_ms = osGetTime();
    LightLock_Unlock(&g_music_io.lock);
    LightEvent_Signal(&g_music_io.output_wake);
    LightEvent_Signal(&g_music_io.network_wake);
}

static bool music_init_dsp(void)
{
    /* Allocation and ndspInit happen on the UI thread before workers start.
     * libctru's shared linear allocator is also used by artwork textures. */
    if (!g_music.dsp) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO); ndspSetClippingMode(NDSP_CLIP_SOFT);
    ndspChnReset(0); ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetFormat(0, g_music.stream.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(0, g_music.stream.rate);
    ndspChnSetPaused(0, true);
    for (int i = 0; i < MUSIC_BUFFER_COUNT; ++i) {
        if (!g_music.pcm[i]) return false;
        memset(&g_music.wave[i], 0, sizeof(ndspWaveBuf));
    }
    g_music.output_ready = true;
    return true;
}

#include "music_worker.inc"

static void music_skip(int direction, bool natural)
{
    int index = g_music.index + direction;
    music_release(natural, index < g_music.count);
    if (index < 0) index = 0;
    if (index >= g_music.count) { g_music.phase = MUSIC_ENDED; g_music.buffering = false; return; }
    g_music.index = index;
    if (g_music.queue[index] >= 0) g_current = g_items[g_music.queue[index]];
    g_music.paused = false;
    music_open(0);
}

static void music_seek(u64 ticks)
{
    if (g_current.runtime_ticks && ticks >= g_current.runtime_ticks)
        ticks = g_current.runtime_ticks > TICKS_PER_SECOND ? g_current.runtime_ticks - TICKS_PER_SECOND : 0;
    music_release(false, true);
    music_open(ticks);
}

static void music_tick(void)
{
    if (!g_music.active || g_music.phase == MUSIC_ERROR || g_music.phase == MUSIC_ENDED) return;
    if (g_music.phase == MUSIC_INFO) {
        int running, remaining;
        CURLMcode result = curl_multi_perform(g_music.multi, &running);
        CURLMsg *message = curl_multi_info_read(g_music.multi, &remaining);
        if (result != CURLM_OK) { music_fail("Connection interrupted. Press A to retry."); return; }
        if (!message || message->msg != CURLMSG_DONE) return;
        long status = 0;
        curl_easy_getinfo(g_music.http, CURLINFO_RESPONSE_CODE, &status);
        if (message->data.result != CURLE_OK || status != 200) {
            music_fail("Could not prepare this song. Press A to retry."); return;
        }
        const char *body = g_music.info.data, *end = body ? body + g_music.info.size : NULL;
        const char *source = NULL, *source_end = NULL, *array = body ? find_key_range(body, end, "MediaSources") : NULL;
        if (array && *array == '[') json_object_range_after(array + 1, end, &source, &source_end);
        if (body) json_get_string_range(body, end, "PlaySessionId", g_play_session, sizeof(g_play_session));
        if (source) json_get_string_range(source, source_end, "Id", g_play_media_source_id, sizeof(g_play_media_source_id));
        bool allowed = true;
        if (source) json_get_bool_range(source, source_end, "SupportsTranscoding", &allowed);
        if (!source || !allowed || !g_play_session[0] || !g_play_media_source_id[0]) {
            music_fail("This song cannot be streamed. Check audio transcoding permissions."); return;
        }
        music_detach_http();
        size_t capacity = (size_t)g_music.sound.buffer_kb * 1024;
        if (g_music.stream.capacity != capacity) {
            free(g_music.stream.data);
            g_music.stream.data = NULL;
            g_music.stream.capacity = capacity;
        }
        if (!g_music.stream.data) g_music.stream.data = malloc(capacity);
        if (!g_music.stream.data) { music_fail("Not enough memory to play music."); return; }
        char path[1600]; music_build_stream_path(path, sizeof(path), &g_current, g_music.base_ticks, &g_music.sound);
        g_music.phase = MUSIC_STREAM;
        if (!music_http_begin(path, false) || !music_workers_start()) music_fail("Could not start audio streaming. Press A to retry.");
        return;
    }
    MusicOutputState output;
    char error[160];
    LightLock_Lock(&g_music_io.lock);
    output = g_music_io.output;
    copy_safe(error, sizeof(error), g_music_io.error);
    LightLock_Unlock(&g_music_io.lock);
    g_music.buffering = output.buffering;
    g_music.audio_rate = output.rate; g_music.audio_channels = output.channels;
    g_music.buffered_bytes = output.buffered_bytes; g_music.underruns = output.underruns;
    if (error[0]) { music_fail(error); return; }
    if (!g_music.started && output.started) {
        g_music.started = true;
        report_playback_start(g_music.base_ticks, g_music.paused, g_music.muted, g_cfg.music.volume_percent);
    }
    if (output.finished) { music_skip(1, true); return; }
    report_playback_progress_periodic(output.position, g_music.paused || output.buffering, g_music.muted, g_cfg.music.volume_percent);
}

static void music_toggle_pause(void)
{
    if (g_music.phase == MUSIC_ERROR || g_music.phase == MUSIC_ENDED) { g_music.paused = false; music_seek(0); return; }
    g_music.paused = !g_music.paused;
    music_apply_volume();
    if (g_music.started) report_playback_progress(music_position(), g_music.paused, g_music.muted, g_cfg.music.volume_percent);
}

static void open_music_settings(void)
{
    g_music_settings_draft = g_cfg.music;
    music_sound_normalize(&g_music_settings_draft);
    g_music_settings_return_view = g_view;
    g_music_settings_row = 0;
    g_view = VIEW_MUSIC_SETTINGS;
}

static void music_save_settings(void)
{
    music_sound_normalize(&g_music_settings_draft);
    bool restart = g_music_settings_draft.sample_rate != g_cfg.music.sample_rate ||
        g_music_settings_draft.channels != g_cfg.music.channels || g_music_settings_draft.buffer_kb != g_cfg.music.buffer_kb;
    bool volume_changed = g_music_settings_draft.volume_percent != g_cfg.music.volume_percent;
    g_cfg.music = g_music_settings_draft;
    save_config();
    g_music.volume_dirty = false;
    if (g_music.active) {
        if (volume_changed) g_music.muted = g_cfg.music.volume_percent == 0;
        if (restart && (g_music.phase == MUSIC_STREAM || g_music.phase == MUSIC_INFO)) music_seek(music_position());
        else music_apply_volume();
    }
    g_view = g_music_settings_return_view;
}

static void music_change_setting(int direction)
{
    MusicSoundSettings *sound = &g_music_settings_draft;
    if (g_music_settings_row == 0) {
        const int rates[] = {32000, 44100, 48000};
        int index = sound->sample_rate == 32000 ? 0 : sound->sample_rate == 44100 ? 1 : 2;
        sound->sample_rate = rates[(index + direction + 3) % 3];
    } else if (g_music_settings_row == 1) sound->channels = sound->channels == 2 ? 1 : 2;
    else if (g_music_settings_row == 2) {
        const int sizes[] = {128, 512, 1024};
        int index = sound->buffer_kb == 128 ? 0 : sound->buffer_kb == 512 ? 1 : 2;
        sound->buffer_kb = sizes[(index + direction + 3) % 3];
    } else if (g_music_settings_row == 3) sound->volume_percent = clamp_int(sound->volume_percent + direction * 5, 0, 100);
    else if (g_music_settings_row == 4) *sound = music_sound_defaults();
    else music_save_settings();
}

static void handle_music_settings(u32 down)
{
    if (down & KEY_B) { g_view = g_music_settings_return_view; return; }
    if (down & KEY_SELECT) { music_save_settings(); return; }
    if (down & KEY_UP) g_music_settings_row = (g_music_settings_row + 5) % 6;
    if (down & KEY_DOWN) g_music_settings_row = (g_music_settings_row + 1) % 6;
    if (down & KEY_LEFT) music_change_setting(-1);
    else if (down & (KEY_RIGHT | KEY_A)) music_change_setting(1);
    if (down & KEY_TOUCH) {
        touchPosition p; hidTouchRead(&p);
        if (p.py >= 34 && p.py < 162 && p.px >= 10 && p.px <= 310) {
            g_music_settings_row = (p.py - 34) / 32;
            music_change_setting(p.px < 160 ? -1 : 1);
        } else if (p.py >= 174 && p.py <= 207) {
            g_music_settings_row = p.px < 160 ? 4 : 5;
            music_change_setting(1);
        } else if (p.py >= 216) g_view = g_music_settings_return_view;
    }
}

static void handle_music_input(u32 down)
{
    if (down & KEY_SELECT) { open_music_settings(); return; }
    if (down & KEY_B) { View back = g_music.return_view; music_stop(); g_view = back; return; }
    if (down & KEY_A) music_toggle_pause();
    if (down & KEY_R) { music_skip(1, false); return; }
    if (down & KEY_L) { if (music_position() > 3 * TICKS_PER_SECOND) music_seek(0); else music_skip(-1, false); return; }
    if (down & (KEY_LEFT | KEY_RIGHT)) {
        u64 at = music_position();
        music_seek(down & KEY_RIGHT ? at + 10 * TICKS_PER_SECOND : at > 10 * TICKS_PER_SECOND ? at - 10 * TICKS_PER_SECOND : 0);
        return;
    }
    if (down & (KEY_UP | KEY_DOWN)) {
        g_cfg.music.volume_percent = clamp_int(g_cfg.music.volume_percent + ((down & KEY_UP) ? 5 : -5), 0, 100);
        g_music.volume_dirty = true;
        g_music.muted = g_cfg.music.volume_percent == 0;
        music_apply_volume();
    }
    if (down & KEY_Y) { g_music.muted = !g_music.muted; music_apply_volume(); }
    if (down & KEY_X) toggle_favorite(&g_current);
    if (down & KEY_TOUCH) {
        touchPosition p; hidTouchRead(&p);
        if (p.py >= 90 && p.py <= 117 && p.px >= 20 && p.px <= 300 && g_current.runtime_ticks) {
            music_seek((u64)((double)(p.px - 20) / 280 * g_current.runtime_ticks)); return;
        }
        if (p.py >= 130 && p.py < 176) {
            if (p.px < 100) { if (music_position() > 3 * TICKS_PER_SECOND) music_seek(0); else music_skip(-1, false); }
            else if (p.px > 220) music_skip(1, false);
            else music_toggle_pause();
        } else if (p.py >= 182 && p.py < 209) {
            if (p.px < 110) toggle_favorite(&g_current);
            else if (p.px < 210) { g_music.muted = !g_music.muted; music_apply_volume(); }
            else open_music_settings();
        } else if (p.py >= 213) {
            View back = g_music.return_view; music_stop(); g_view = back;
        }
    }
}

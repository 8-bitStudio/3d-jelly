/* Independent transport and DSP feeder: rendering, JPEG decoding, favorites,
 * config writes and server progress reports cannot starve the sound queue. */
static void music_io_fail(const char *message)
{
    LightLock_Lock(&g_music_io.lock);
    if (!g_music_io.error[0]) copy_safe(g_music_io.error, sizeof(g_music_io.error), message);
    LightLock_Unlock(&g_music_io.lock);
    LightEvent_Signal(&g_music_io.output_wake);
}

static void music_network_worker(void *unused)
{
    (void)unused;
    for (;;) {
        LightLock_Lock(&g_music_io.lock);
        bool stop = g_music_io.stop || g_music_io.error[0];
        size_t refill = g_music.stream.capacity / 8;
        if (refill < 2 * CURL_MAX_WRITE_SIZE) refill = 2 * CURL_MAX_WRITE_SIZE;
        bool resume = g_music_io.flow_paused && g_music.stream.capacity - g_music.stream.used >= refill;
        if (resume) { g_music_io.flow_paused = false; g_music_io.last_data_ms = osGetTime(); }
        bool full = g_music_io.flow_paused;
        bool stalled = !full && !g_music_io.paused && osGetTime() - g_music_io.last_data_ms > 20000;
        LightLock_Unlock(&g_music_io.lock);
        if (stop) break;
        if (stalled) { music_io_fail("Connection stalled. Press A to retry."); break; }
        if (full) { LightEvent_WaitTimeout(&g_music_io.network_wake, 50000000LL); continue; }
        if (resume && curl_easy_pause(g_music.http, CURLPAUSE_CONT) != CURLE_OK) {
            music_io_fail("Audio connection interrupted. Press A to retry."); break;
        }
        int running = 0, remaining = 0;
        if (curl_multi_perform(g_music.multi, &running) != CURLM_OK) {
            music_io_fail("Audio connection interrupted. Press A to retry."); break;
        }
        CURLMsg *message = curl_multi_info_read(g_music.multi, &remaining);
        if (message && message->msg == CURLMSG_DONE) {
            long status = 0;
            curl_easy_getinfo(g_music.http, CURLINFO_RESPONSE_CODE, &status);
            if (message->data.result != CURLE_OK || status != 200) {
                music_io_fail("Audio download interrupted. Press A to retry.");
            } else {
                LightLock_Lock(&g_music_io.lock);
                g_music_io.net_done = true;
                LightLock_Unlock(&g_music_io.lock);
                LightEvent_Signal(&g_music_io.output_wake);
            }
            break;
        }
        int descriptors = 0;
        if (curl_multi_wait(g_music.multi, NULL, 0, 15, &descriptors) != CURLM_OK) {
            music_io_fail("Audio connection interrupted. Press A to retry."); break;
        }
        /* Some connection phases have no socket to wait on. Avoid a busy loop. */
        if (!descriptors) svcSleepThread(1000000LL);
    }
}

static void music_output_publish(const MusicOutputState *output)
{
    LightLock_Lock(&g_music_io.lock);
    g_music_io.output = *output;
    LightLock_Unlock(&g_music_io.lock);
}

static void music_output_worker(void *unused)
{
    (void)unused;
    MusicOutputState output = {.buffering = true, .position = g_music.base_ticks};
    int last_volume = -1;
    bool output_paused = true;
    for (;;) {
        LightLock_Lock(&g_music_io.lock);
        bool stop = g_music_io.stop || g_music_io.error[0];
        bool net_done = g_music_io.net_done, paused = g_music_io.paused;
        int volume = g_music_io.muted ? 0 : g_music_io.volume;
        size_t before = g_music.stream.used;
        bool ready = !stop && music_wav_header(&g_music.stream);
        bool invalid = g_music.stream.failed || (!ready && net_done && before == g_music.stream.used);
        LightLock_Unlock(&g_music_io.lock);
        if (stop) break;
        if (invalid) { music_io_fail("The server did not return supported PCM audio."); break; }
        if (!ready) { LightEvent_WaitTimeout(&g_music_io.output_wake, 10000000LL); continue; }
        if (!g_music.output_ready && !music_init_dsp()) {
            music_io_fail("Audio initialization failed. Check that DSP firmware is installed."); break;
        }
        output.rate = g_music.stream.rate; output.channels = g_music.stream.channels;
        if (volume != last_volume) {
            float mix[12] = {0};
            mix[0] = mix[1] = volume / 100.0f;
            ndspChnSetMix(0, mix);
            last_volume = volume;
        }

        int live = 0;
        for (int i = 0; i < MUSIC_BUFFER_COUNT; ++i) {
            if (g_music.queued[i] && g_music.wave[i].status == NDSP_WBUF_DONE) {
                g_music.completed_frames += g_music.wave[i].nsamples;
                g_music.queued[i] = false;
            }
            live += g_music.queued[i];
        }
        LightLock_Lock(&g_music_io.lock);
        size_t available = music_pcm_available(&g_music.stream);
        LightLock_Unlock(&g_music_io.lock);
        if (output.started && !output.buffering && !paused && !live && (!net_done || available)) {
            output.buffering = true;
            ++output.underruns;
            ndspChnSetPaused(0, true); output_paused = true;
        }
        size_t queued_frames = 0;
        int queued = 0;
        for (int i = 0; i < MUSIC_BUFFER_COUNT; ++i) {
            if (!g_music.queued[i]) {
                size_t wanted = MUSIC_BUFFER_FRAMES * g_music.stream.frame_bytes;
                LightLock_Lock(&g_music_io.lock);
                size_t bytes = music_pcm_available(&g_music.stream);
                bool tail = net_done || (g_music.stream.limited && g_music.stream.remaining <= wanted);
                bytes = bytes >= wanted || (bytes && tail) ? music_pcm_read(&g_music.stream, (u8 *)g_music.pcm[i], wanted) : 0;
                bool wake_network = g_music_io.flow_paused;
                LightLock_Unlock(&g_music_io.lock);
                if (bytes) {
                    g_music.wave[i].data_pcm16 = g_music.pcm[i];
                    g_music.wave[i].nsamples = bytes / g_music.stream.frame_bytes;
                    DSP_FlushDataCache(g_music.pcm[i], bytes);
                    ndspChnWaveBufAdd(0, &g_music.wave[i]);
                    g_music.queued[i] = true;
                    if (wake_network) LightEvent_Signal(&g_music_io.network_wake);
                }
            }
            if (g_music.queued[i]) { ++queued; queued_frames += g_music.wave[i].nsamples; }
        }
        LightLock_Lock(&g_music_io.lock);
        size_t buffered_frames = queued_frames + music_pcm_available(&g_music.stream) / g_music.stream.frame_bytes;
        bool truncated = (g_music.stream.limited && g_music.stream.remaining) ||
            (!g_music.stream.limited && g_music.stream.used) || !g_music.completed_frames;
        LightLock_Unlock(&g_music_io.lock);
        output.position = music_output_position();
        output.buffered_bytes = buffered_frames * g_music.stream.frame_bytes;
        if (!queued && net_done) {
            if (truncated) music_io_fail("The audio stream ended early. Press A to retry.");
            else { output.finished = true; output.buffering = false; }
            music_output_publish(&output);
            break;
        }
        /* A real reserve before starting; repeated underruns build more reserve.
         * Short songs / the final tail play as soon as the download is complete. */
        unsigned reserve_ms = output.started ? 1000 + output.underruns * 500 : 1000;
        if (reserve_ms > 3500) reserve_ms = 3500;
        size_t reserve_frames = (size_t)g_music.stream.rate * reserve_ms / 1000;
        size_t maximum = g_music.stream.capacity / (2 * g_music.stream.frame_bytes);
        if (reserve_frames > maximum) reserve_frames = maximum;
        if (output.buffering && queued && (buffered_frames >= reserve_frames || net_done)) {
            output.buffering = false;
            output.started = true;
        }
        bool should_pause = paused || output.buffering;
        if (should_pause != output_paused) { ndspChnSetPaused(0, should_pause); output_paused = should_pause; }
        music_output_publish(&output);
        /* A control change signals this event, so paused playback can sleep
         * longer without adding resume/volume latency. */
        LightEvent_WaitTimeout(&g_music_io.output_wake, paused ? 100000000LL : 8000000LL);
    }
    if (g_music.dsp) ndspChnSetPaused(0, true);
    output.position = music_output_position();
    music_output_publish(&output);
}

static void music_workers_stop(void)
{
    if (!g_music_io.running) return;
    LightLock_Lock(&g_music_io.lock);
    g_music_io.stop = true;
    LightLock_Unlock(&g_music_io.lock);
    LightEvent_Signal(&g_music_io.network_wake);
    LightEvent_Signal(&g_music_io.output_wake);
    if (g_music_io.network_thread) { threadJoin(g_music_io.network_thread, U64_MAX); threadFree(g_music_io.network_thread); }
    if (g_music_io.output_thread) { threadJoin(g_music_io.output_thread, U64_MAX); threadFree(g_music_io.output_thread); }
    g_music_io.network_thread = g_music_io.output_thread = NULL;
    g_music_io.running = false;
}

static bool music_workers_start(void)
{
    /* Keep the non-thread-safe linear allocator on the same thread as C3D. */
    if (!g_music.dsp && R_FAILED(ndspInit())) return false;
    g_music.dsp = true;
    ndspChnReset(0); ndspChnSetPaused(0, true);
    for (int i = 0; i < MUSIC_BUFFER_COUNT; ++i) {
        if (!g_music.pcm[i]) g_music.pcm[i] = linearAlloc(MUSIC_BUFFER_FRAMES * 4);
        if (!g_music.pcm[i]) return false;
    }
    LightEvent_Clear(&g_music_io.network_wake);
    LightEvent_Clear(&g_music_io.output_wake);
    g_music_io.stop = g_music_io.net_done = g_music_io.flow_paused = false;
    g_music_io.paused = g_music.paused; g_music_io.muted = g_music.muted;
    g_music_io.volume = g_cfg.music.volume_percent;
    g_music_io.last_data_ms = osGetTime();
    g_music_io.error[0] = 0;
    g_music_io.output = (MusicOutputState){.buffering = true, .position = g_music.base_ticks};
    g_music_io.running = true;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    int network_priority = clamp_int(priority - 1, 0x19, 0x3F);
    int output_priority = clamp_int(priority - 2, 0x18, 0x3F);
    if (g_core1_reader_available)
        g_music_io.network_thread = threadCreate(music_network_worker, NULL, 64 * 1024, network_priority, 1, false);
    if (!g_music_io.network_thread)
        g_music_io.network_thread = threadCreate(music_network_worker, NULL, 64 * 1024, network_priority, -2, false);
    if (g_music_io.network_thread)
        g_music_io.output_thread = threadCreate(music_output_worker, NULL, 32 * 1024, output_priority, -2, false);
    if (!g_music_io.network_thread || !g_music_io.output_thread) { music_workers_stop(); return false; }
    return true;
}

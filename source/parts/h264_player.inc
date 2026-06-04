typedef struct {
    int pmt_pid;
    int video_pid;
    size_t carry_size;
    u8 carry[188];
    u8 *h264_buf;
    size_t h264_size;
    u8 *mvd_in;
    u8 *mvd_out;
    MVDSTD_Config config;
    u32 nal_count;
    u32 frame_count;
    u32 byte_count;
    Result last_result;
} StreamPlayer;

static void player_console(const StreamPlayer *player, const char *line)
{
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    consoleClear();
    printf("3dJelly player\n");
    printf("%s\n\n", g_current.name[0] ? g_current.name : "Video");
    printf("%s\n\n", line ? line : "");
    printf("Quality: %s\n", quality_label);
    if (player) {
        printf("PMT PID: %d  H264 PID: %d\n", player->pmt_pid, player->video_pid);
        printf("NAL: %lu  Frames: %lu\n", (unsigned long)player->nal_count, (unsigned long)player->frame_count);
        printf("Bytes: %lu  Last: 0x%08lX\n", (unsigned long)player->byte_count, (unsigned long)player->last_result);
    }
    printf("\nB stop playback\nSTART exit app\n");
}

static Result add_stream_headers(httpcContext *context)
{
    Result ret = 0;
    ret = httpcSetSSLOpt(context, SSLCOPT_DisableVerify);
    if (R_FAILED(ret)) {
        return ret;
    }
    httpcSetKeepAlive(context, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(context, "User-Agent", "3dJelly/" APP_VERSION " Nintendo 3DS");
    httpcAddRequestHeaderField(context, "Accept", "video/mp2t, multipart/x-mixed-replace, image/jpeg, audio/wav, audio/*, */*");
    httpcAddRequestHeaderField(context, "Connection", "Close");

    char auth[384];
    auth_header(auth, sizeof(auth), true);
    httpcAddRequestHeaderField(context, "Authorization", auth);
    httpcAddRequestHeaderField(context, "X-Emby-Authorization", auth);
    if (g_cfg.token[0]) {
        httpcAddRequestHeaderField(context, "X-Emby-Token", g_cfg.token);
    }
    return 0;
}

static Result open_stream_context(httpcContext *context, const char *url, u32 *status_out)
{
    memset(context, 0, sizeof(*context));
    *status_out = HTTP_STATUS_NONE;
    if (!g_http_ready) {
        return (Result)0xD9000003;
    }

    char *active_url = strdup(url);
    char *redirect_url = NULL;
    if (!active_url) {
        return -1;
    }

    Result ret = 0;
    for (int redirects = 0; redirects < 4; redirects++) {
        memset(context, 0, sizeof(*context));
        ret = httpcOpenContext(context, HTTPC_METHOD_GET, active_url, 1);
        if (R_FAILED(ret)) {
            break;
        }
        ret = add_stream_headers(context);
        if (R_FAILED(ret)) {
            httpcCloseContext(context);
            break;
        }
        ret = httpcBeginRequest(context);
        if (R_FAILED(ret)) {
            httpcCloseContext(context);
            break;
        }
        ret = httpcGetResponseStatusCodeTimeout(context, status_out, HTTP_STATUS_TIMEOUT_NS);
        if (R_FAILED(ret)) {
            httpcCancelConnection(context);
            httpcCloseContext(context);
            break;
        }

        if ((*status_out >= 301 && *status_out <= 303) || (*status_out >= 307 && *status_out <= 308)) {
            if (!redirect_url) {
                redirect_url = (char *)malloc(1024);
            }
            if (!redirect_url) {
                ret = -1;
                httpcCancelConnection(context);
                httpcCloseContext(context);
                break;
            }

            memset(redirect_url, 0, 1024);
            ret = httpcGetResponseHeader(context, "Location", redirect_url, 1024);
            httpcCancelConnection(context);
            httpcCloseContext(context);
            if (R_FAILED(ret) || !redirect_url[0]) {
                break;
            }

            char full[STREAM_URL_CAP];
            build_url(full, sizeof(full), redirect_url);
            free(active_url);
            active_url = strdup(full);
            if (!active_url) {
                ret = -1;
                break;
            }
            continue;
        }

        if (*status_out < 200 || *status_out >= 300) {
            httpcCancelConnection(context);
            httpcCloseContext(context);
            ret = (Result)0xD9000002;
        }
        break;
    }

    free(active_url);
    free(redirect_url);
    return ret;
}

static Result stream_receive_chunk(httpcContext *context, u8 *buffer, u32 size, u32 *read_size)
{
    u32 start = 0;
    u32 end = 0;
    *read_size = 0;

    Result ret = httpcGetDownloadSizeState(context, &start, NULL);
    if (R_FAILED(ret)) {
        return ret;
    }

    ret = httpcReceiveDataTimeout(context, buffer, size, STREAM_READ_TIMEOUT_NS);

    Result state_ret = httpcGetDownloadSizeState(context, &end, NULL);
    if (R_SUCCEEDED(state_ret) && end >= start) {
        *read_size = end - start;
        if (*read_size > size) {
            *read_size = size;
        }
    } else if (R_FAILED(state_ret) && R_SUCCEEDED(ret)) {
        return state_ret;
    }

    return ret;
}

static bool find_h264_start(const u8 *buf, size_t len, size_t from, size_t *pos, size_t *prefix)
{
    if (len < 3 || from >= len) {
        return false;
    }
    for (size_t i = from; i + 3 <= len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            *pos = i;
            *prefix = 3;
            return true;
        }
        if (i + 4 <= len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
            *pos = i;
            *prefix = 4;
            return true;
        }
    }
    return false;
}

static bool process_h264_nal(StreamPlayer *player, const u8 *data, size_t size)
{
    if (size == 0) {
        return true;
    }
    if (size > MVD_IN_CAP) {
        set_play_status("H.264 NAL too large: %lu bytes.", (unsigned long)size);
        return false;
    }

    memcpy(player->mvd_in, data, size);
    GSPGPU_FlushDataCache(player->mvd_in, size);

    MVDSTD_ProcessNALUnitOut out;
    memset(&out, 0, sizeof(out));
    Result ret = mvdstdProcessVideoFrame(player->mvd_in, size, 0, &out);
    player->last_result = ret;
    player->nal_count++;

    if (!MVD_CHECKNALUPROC_SUCCESS(ret)) {
        set_play_status("MVD decode failed: 0x%08lX.", (unsigned long)ret);
        return false;
    }

    if (ret != MVD_STATUS_PARAMSET && ret != MVD_STATUS_INCOMPLETEPROCESSING) {
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        player->config.physaddr_outdata0 = osConvertVirtToPhys(fb);
        ret = mvdstdRenderVideoFrame(&player->config, true);
        player->last_result = ret;
        if (ret != MVD_STATUS_OK) {
            set_play_status("MVD render failed: 0x%08lX.", (unsigned long)ret);
            return false;
        }
        gfxSwapBuffersGpu();
        player->frame_count++;
    }

    return true;
}

static bool append_h264_bytes(StreamPlayer *player, const u8 *data, size_t size)
{
    if (!size) {
        return true;
    }
    if (size > H264_BUFFER_CAP - player->h264_size) {
        player->h264_size = 0;
        set_play_status("H.264 stream buffer overflow; dropping pending data.");
        return true;
    }

    memcpy(player->h264_buf + player->h264_size, data, size);
    player->h264_size += size;

    while (player->h264_size > 4) {
        size_t first = 0;
        size_t first_prefix = 0;
        if (!find_h264_start(player->h264_buf, player->h264_size, 0, &first, &first_prefix)) {
            size_t keep = player->h264_size < 4 ? player->h264_size : 4;
            memmove(player->h264_buf, player->h264_buf + player->h264_size - keep, keep);
            player->h264_size = keep;
            return true;
        }

        if (first > 0) {
            memmove(player->h264_buf, player->h264_buf + first, player->h264_size - first);
            player->h264_size -= first;
            first = 0;
        }

        size_t second = 0;
        size_t second_prefix = 0;
        (void)second_prefix;
        if (!find_h264_start(player->h264_buf, player->h264_size, first_prefix, &second, &second_prefix)) {
            return true;
        }

        size_t nal_start = first_prefix == 4 ? 1 : 0;
        size_t nal_size = second - nal_start;
        if (nal_size > 0 && !process_h264_nal(player, player->h264_buf + nal_start, nal_size)) {
            return false;
        }

        memmove(player->h264_buf, player->h264_buf + second, player->h264_size - second);
        player->h264_size -= second;
    }

    return true;
}

static void parse_pat(StreamPlayer *player, const u8 *payload, size_t len, bool pusi)
{
    if (pusi) {
        if (len < 1) {
            return;
        }
        size_t pointer = payload[0];
        if (pointer + 1 >= len) {
            return;
        }
        payload += pointer + 1;
        len -= pointer + 1;
    }
    if (len < 12 || payload[0] != 0x00) {
        return;
    }

    size_t section_length = ((payload[1] & 0x0F) << 8) | payload[2];
    size_t section_end = 3 + section_length;
    if (section_end > len) {
        section_end = len;
    }
    if (section_end < 12) {
        return;
    }
    section_end -= 4; /* CRC */

    for (size_t pos = 8; pos + 4 <= section_end; pos += 4) {
        u16 program = ((u16)payload[pos] << 8) | payload[pos + 1];
        int pid = ((payload[pos + 2] & 0x1F) << 8) | payload[pos + 3];
        if (program != 0) {
            player->pmt_pid = pid;
            return;
        }
    }
}

static void parse_pmt(StreamPlayer *player, const u8 *payload, size_t len, bool pusi)
{
    if (pusi) {
        if (len < 1) {
            return;
        }
        size_t pointer = payload[0];
        if (pointer + 1 >= len) {
            return;
        }
        payload += pointer + 1;
        len -= pointer + 1;
    }
    if (len < 16 || payload[0] != 0x02) {
        return;
    }

    size_t section_length = ((payload[1] & 0x0F) << 8) | payload[2];
    size_t section_end = 3 + section_length;
    if (section_end > len) {
        section_end = len;
    }
    if (section_end < 16) {
        return;
    }
    section_end -= 4; /* CRC */

    size_t program_info_length = ((payload[10] & 0x0F) << 8) | payload[11];
    size_t pos = 12 + program_info_length;
    while (pos + 5 <= section_end) {
        u8 stream_type = payload[pos];
        int pid = ((payload[pos + 1] & 0x1F) << 8) | payload[pos + 2];
        size_t es_info_length = ((payload[pos + 3] & 0x0F) << 8) | payload[pos + 4];
        if (stream_type == 0x1B) {
            player->video_pid = pid;
            return;
        }
        pos += 5 + es_info_length;
    }
}

static bool parse_video_pes(StreamPlayer *player, const u8 *payload, size_t len, bool pusi)
{
    if (pusi && len >= 9 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
        size_t header_len = 9 + payload[8];
        if (header_len >= len) {
            return true;
        }
        payload += header_len;
        len -= header_len;
    }

    return append_h264_bytes(player, payload, len);
}

static bool handle_ts_packet(StreamPlayer *player, const u8 *pkt)
{
    if (pkt[0] != 0x47) {
        return true;
    }

    bool pusi = (pkt[1] & 0x40) != 0;
    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    int afc = (pkt[3] >> 4) & 0x03;
    size_t pos = 4;

    if (afc == 0 || afc == 2) {
        return true;
    }
    if (afc == 3) {
        if (pos >= 188) {
            return true;
        }
        pos += 1 + pkt[pos];
        if (pos >= 188) {
            return true;
        }
    }

    const u8 *payload = pkt + pos;
    size_t len = 188 - pos;

    if (pid == 0) {
        parse_pat(player, payload, len, pusi);
    } else if (player->pmt_pid >= 0 && pid == player->pmt_pid) {
        parse_pmt(player, payload, len, pusi);
    } else if (player->video_pid >= 0 && pid == player->video_pid) {
        return parse_video_pes(player, payload, len, pusi);
    }

    return true;
}

static bool feed_ts_bytes(StreamPlayer *player, const u8 *data, size_t size)
{
    player->byte_count += (u32)size;

    if (player->carry_size) {
        size_t need = 188 - player->carry_size;
        if (need > size) {
            memcpy(player->carry + player->carry_size, data, size);
            player->carry_size += size;
            return true;
        }
        memcpy(player->carry + player->carry_size, data, need);
        if (!handle_ts_packet(player, player->carry)) {
            return false;
        }
        data += need;
        size -= need;
        player->carry_size = 0;
    }

    while (size >= 188) {
        if (data[0] != 0x47) {
            size_t sync = 0;
            while (sync < size && data[sync] != 0x47) {
                sync++;
            }
            data += sync;
            size -= sync;
            if (size < 188) {
                break;
            }
        }
        if (!handle_ts_packet(player, data)) {
            return false;
        }
        data += 188;
        size -= 188;
    }

    if (size) {
        memcpy(player->carry, data, size);
        player->carry_size = size;
    }

    return true;
}

static bool player_init(StreamPlayer *player)
{
    memset(player, 0, sizeof(*player));
    player->pmt_pid = -1;
    player->video_pid = -1;

    player->h264_buf = (u8 *)malloc(H264_BUFFER_CAP);
    player->mvd_in = (u8 *)linearMemAlign(MVD_IN_CAP, 0x40);
    player->mvd_out = (u8 *)linearMemAlign(MVD_OUT_CAP, 0x40);
    if (!player->h264_buf || !player->mvd_in || !player->mvd_out) {
        set_play_status("Not enough memory for video playback buffers.");
        return false;
    }

    if (!g_is_new_3ds) {
        set_play_status("Video playback needs New3DS MVD hardware.");
        return false;
    }

    Result ret = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    player->last_result = ret;
    if (R_FAILED(ret)) {
        set_play_status("Could not start MVD decoder: 0x%08lX.", (unsigned long)ret);
        return false;
    }

    QualityProfile q = quality_profile();
    mvdstdGenerateDefaultConfig(&player->config,
                                (u32)q.height,
                                (u32)q.width,
                                (u32)q.height,
                                (u32)q.width,
                                NULL,
                                (u32 *)player->mvd_out,
                                (u32 *)player->mvd_out);
    return true;
}

static void player_free(StreamPlayer *player, bool mvd_started)
{
    if (mvd_started) {
        mvdstdExit();
    }
    if (player->h264_buf) {
        free(player->h264_buf);
    }
    if (player->mvd_in) {
        linearFree(player->mvd_in);
    }
    if (player->mvd_out) {
        linearFree(player->mvd_out);
    }
}

static bool play_stream_url(const char *url)
{
    if (!url || !url[0]) {
        set_play_status("No playback URL.");
        return false;
    }

    ui_graphics_exit();
    gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false);
    consoleInit(GFX_BOTTOM, NULL);

    StreamPlayer player;
    bool ok = false;
    bool mvd_started = false;
    bool remote_started = false;
    player_console(NULL, "Starting decoder...");
    if (!player_init(&player)) {
        player_free(&player, false);
        playback_graphics_exit();
        return false;
    }
    mvd_started = true;
    player_console(&player, "Opening Jellyfin stream...");

    httpcContext context;
    u32 status = HTTP_STATUS_NONE;
    Result ret = open_stream_context(&context, url, &status);
    if (R_FAILED(ret)) {
        set_play_status("Stream open failed: HTTP %lu result 0x%08lX.", (unsigned long)status, (unsigned long)ret);
        if (!app_system_closing()) {
            player_console(&player, g_play_status);
        }
        app_wait_or_exit(1800000000ULL);
        player_free(&player, mvd_started);
        playback_graphics_exit();
        return false;
    }

    u8 *chunk = (u8 *)malloc(STREAM_READ_SIZE);
    bool h264_report_started = false;
    u64 h264_report_start_ms = 0;
    u64 h264_paused_total_ms = 0;
    u64 h264_pause_started_ms = 0;
    bool h264_paused = false;
    remote_started = remote_control_start();
    if (remote_started) {
        g_session_capabilities_reported = false;
    }
    if (!chunk) {
        set_play_status("Could not allocate stream read buffer.");
    } else {
        h264_report_start_ms = osGetTime();
        h264_report_started = true;
        report_playback_start(0, false, false, g_cfg.volume_percent);
        set_play_status("Playing. Press B to stop.");
        player_console(&player, g_play_status);
        u64 last_console_ms = osGetTime();
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
                ok = true;
                break;
            }

            u64 now_ms = osGetTime();
            u64 paused_total = h264_paused_total_ms;
            if (h264_paused && h264_pause_started_ms && now_ms > h264_pause_started_ms) {
                paused_total += now_ms - h264_pause_started_ms;
            }
            u64 elapsed_ms = now_ms > h264_report_start_ms + paused_total ? now_ms - h264_report_start_ms - paused_total : 0;
            u64 position_ticks = clamp_media_ticks(elapsed_ms * (TICKS_PER_SECOND / 1000ULL));
            RemoteCommand remote_cmd;
            if (remote_control_poll(&remote_cmd)) {
                if (remote_cmd.type == REMOTE_CMD_STOP) {
                    set_play_status("Stopped from Jellyfin dashboard.");
                    ok = true;
                    break;
                } else if (remote_cmd.type == REMOTE_CMD_PAUSE) {
                    if (!h264_paused) {
                        h264_pause_started_ms = now_ms;
                        h264_paused = true;
                    }
                    set_play_status("Paused from Jellyfin dashboard.");
                    report_playback_progress(position_ticks, true, false, g_cfg.volume_percent);
                    player_console(&player, g_play_status);
                } else if (remote_cmd.type == REMOTE_CMD_UNPAUSE) {
                    if (h264_paused && h264_pause_started_ms && now_ms > h264_pause_started_ms) {
                        h264_paused_total_ms += now_ms - h264_pause_started_ms;
                    }
                    h264_pause_started_ms = 0;
                    h264_paused = false;
                    set_play_status("Playing. Press B to stop.");
                    report_playback_progress(position_ticks, false, false, g_cfg.volume_percent);
                    player_console(&player, g_play_status);
                } else if (remote_cmd.type == REMOTE_CMD_TOGGLE_PAUSE) {
                    if (h264_paused) {
                        if (h264_pause_started_ms && now_ms > h264_pause_started_ms) {
                            h264_paused_total_ms += now_ms - h264_pause_started_ms;
                        }
                        h264_pause_started_ms = 0;
                        h264_paused = false;
                    } else {
                        h264_pause_started_ms = now_ms;
                        h264_paused = true;
                    }
                    set_play_status(h264_paused ? "Paused from Jellyfin dashboard." : "Playing. Press B to stop.");
                    report_playback_progress(position_ticks, h264_paused, false, g_cfg.volume_percent);
                    player_console(&player, g_play_status);
                } else if (remote_cmd.type == REMOTE_CMD_MESSAGE) {
                    set_play_status("Jellyfin message: %.120s", remote_cmd.text);
                    player_console(&player, g_play_status);
                }
            }

            report_playback_progress_periodic(position_ticks, h264_paused, false, g_cfg.volume_percent);
            if (h264_paused) {
                svcSleepThread(50000000ULL);
                continue;
            }

            u32 read_size = 0;
            ret = stream_receive_chunk(&context, chunk, STREAM_READ_SIZE, &read_size);
            if (read_size && !feed_ts_bytes(&player, chunk, read_size)) {
                break;
            }
            if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING || ret == (s32)HTTPC_RESULTCODE_TIMEDOUT) {
                u64 now_ms = osGetTime();
                if (now_ms - last_console_ms >= 1000) {
                    player_console(&player, "Playing. Press B to stop.");
                    last_console_ms = now_ms;
                }
                continue;
            }
            if (R_FAILED(ret)) {
                set_play_status("Stream read failed: 0x%08lX.", (unsigned long)ret);
                break;
            }
            set_play_status("Playback reached end of stream.");
            ok = true;
            break;
        }
        free(chunk);
    }

    if (h264_report_started) {
        u64 now_ms = osGetTime();
        u64 paused_total = h264_paused_total_ms;
        if (h264_paused && h264_pause_started_ms && now_ms > h264_pause_started_ms) {
            paused_total += now_ms - h264_pause_started_ms;
        }
        u64 elapsed_ms = now_ms > h264_report_start_ms + paused_total ? now_ms - h264_report_start_ms - paused_total : 0;
        u64 position_ticks = clamp_media_ticks(elapsed_ms * (TICKS_PER_SECOND / 1000ULL));
        report_playback_stopped(position_ticks);
    }
    if (!app_system_closing() && (h264_report_started || remote_started)) {
        remote_control_stop();
    }

    if (!app_system_closing()) {
        httpcCancelConnection(&context);
        httpcCloseContext(&context);
    }

    if (!ok && !g_play_status[0]) {
        set_play_status("Playback failed.");
    }
    if (!app_system_closing()) {
        player_console(&player, g_play_status);
    }
    if (!g_exit_requested) {
        app_wait_or_exit(900000000ULL);
    }
    if (!app_system_closing()) {
        player_free(&player, mvd_started);
    }
    playback_graphics_exit();
    return ok;
}

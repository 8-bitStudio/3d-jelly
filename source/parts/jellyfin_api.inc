static bool media_item_is_leaf_media_type(const MediaItem *item)
{
    if (!item) {
        return false;
    }
    return strcmp(item->type, "Movie") == 0 ||
           strcmp(item->type, "Episode") == 0 ||
           strcmp(item->type, "Video") == 0 ||
           strcmp(item->type, "MusicVideo") == 0;
}

static bool media_item_is_unowned_placeholder(const MediaItem *item)
{
    if (!item) {
        return true;
    }
    if (item->is_missing || item->is_virtual_item || item->is_place_holder) {
        return true;
    }
    if (!item->is_folder && strcmp(item->location_type, "Virtual") == 0) {
        return true;
    }
    if (!item->is_folder && media_item_is_leaf_media_type(item) && item->media_source_count == 0) {
        return true;
    }
    return false;
}

static int parse_total_record_count(const char *json)
{
    if (!json) {
        return -1;
    }

    int total = -1;
    const char *end = json + strlen(json);
    if (json_get_int_range(json, end, "TotalRecordCount", &total)) {
        return total;
    }
    return -1;
}

static void parse_media_item_object(const char *os, const char *oe, MediaItem *item)
{
    if (!item) {
        return;
    }

    memset(item, 0, sizeof(*item));
    item->media_source_count = -1;
    item->index_number = -1;
    item->parent_index_number = -1;
    json_get_string_range(os, oe, "Id", item->id, sizeof(item->id));
    json_get_string_range(os, oe, "Name", item->name, sizeof(item->name));
    json_get_string_range(os, oe, "Type", item->type, sizeof(item->type));
    json_get_string_range(os, oe, "CollectionType", item->collection_type, sizeof(item->collection_type));
    json_get_string_range(os, oe, "LocationType", item->location_type, sizeof(item->location_type));
    json_get_string_range(os, oe, "SeriesId", item->series_id, sizeof(item->series_id));
    json_get_string_range(os, oe, "SeasonId", item->season_id, sizeof(item->season_id));
    json_get_bool_range(os, oe, "IsFolder", &item->is_folder);
    json_get_bool_range(os, oe, "IsMissing", &item->is_missing);
    json_get_bool_range(os, oe, "IsVirtualItem", &item->is_virtual_item);
    json_get_bool_range(os, oe, "IsPlaceHolder", &item->is_place_holder);
    json_get_int_range(os, oe, "ProductionYear", &item->year);
    json_get_int_range(os, oe, "IndexNumber", &item->index_number);
    json_get_int_range(os, oe, "ParentIndexNumber", &item->parent_index_number);
    json_get_int_range(os, oe, "MediaSourceCount", &item->media_source_count);
    json_get_ull_range(os, oe, "RunTimeTicks", &item->runtime_ticks);
}

static bool parse_items(const char *json, MediaItem *items, int *count, int capacity, bool owned_only, int *raw_count)
{
    const char *end = json + strlen(json);
    const char *p = find_key_range(json, end, "Items");
    if (raw_count) {
        *raw_count = 0;
    }
    if (!p) {
        return false;
    }
    p = skip_ws(p, end);
    if (p >= end || *p != '[') {
        return false;
    }
    p++;

    int n = *count;
    int raw = 0;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') {
            break;
        }
        if (*p != '{') {
            p++;
            continue;
        }

        const char *os = NULL;
        const char *oe = NULL;
        if (!json_object_range_after(p, end, &os, &oe)) {
            break;
        }
        raw++;

        MediaItem item;
        parse_media_item_object(os, oe, &item);

        if (n < capacity && item.id[0] && item.name[0] && (!owned_only || !media_item_is_unowned_placeholder(&item))) {
            items[n++] = item;
        }
        p = oe;
    }

    *count = n;
    if (raw_count) {
        *raw_count = raw;
    }
    return true;
}

static void free_response(HttpResponse *res)
{
    if (res && res->body) {
        free(res->body);
        res->body = NULL;
    }
}

static void auth_header(char *out, size_t outsz, bool include_token)
{
    if (include_token && g_cfg.token[0]) {
        snprintf(out, outsz,
                 "MediaBrowser Client=\"3dJelly\", Device=\"Nintendo 3DS\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"",
                 g_cfg.device_id, APP_VERSION, g_cfg.token);
    } else {
        snprintf(out, outsz,
                 "MediaBrowser Client=\"3dJelly\", Device=\"Nintendo 3DS\", DeviceId=\"%s\", Version=\"%s\"",
                 g_cfg.device_id, APP_VERSION);
    }
}

static bool http_status_is_redirect(u32 status)
{
    return (status >= 301 && status <= 303) || (status >= 307 && status <= 308);
}

static Result http_redirect_location(httpcContext *context, char *out, size_t outsz)
{
    static const char *const names[] = {"Location", "location", "LOCATION"};
    Result last_ret = (Result)0xD9000002;
    if (!context || !out || outsz == 0) {
        return -1;
    }

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        out[0] = 0;
        Result ret = httpcGetResponseHeader(context, names[i], out, outsz);
        if (R_SUCCEEDED(ret) && out[0]) {
            trim_edges(out);
            if (out[0]) {
                return 0;
            }
        }
        last_ret = ret;
    }
    out[0] = 0;
    return last_ret;
}

static bool url_origin_from(const char *url, char *out, size_t outsz)
{
    const char *scheme_end = url ? strstr(url, "://") : NULL;
    if (!scheme_end || !out || outsz == 0) {
        return false;
    }

    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    size_t len = path ? (size_t)(path - url) : strlen(url);
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, url, len);
    out[len] = 0;
    return len > 0;
}

static bool url_directory_from(const char *url, char *out, size_t outsz)
{
    const char *scheme_end = url ? strstr(url, "://") : NULL;
    if (!scheme_end || !out || outsz == 0) {
        return false;
    }

    const char *host = scheme_end + 3;
    const char *limit = strchr(host, '?');
    if (!limit) {
        limit = url + strlen(url);
    }

    const char *last_slash = NULL;
    for (const char *p = host; p < limit; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }

    if (!last_slash) {
        if (!url_origin_from(url, out, outsz)) {
            return false;
        }
        size_t len = strlen(out);
        if (len + 1 < outsz) {
            out[len] = '/';
            out[len + 1] = 0;
        }
        return true;
    }

    size_t len = (size_t)(last_slash - url + 1);
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, url, len);
    out[len] = 0;
    return len > 0;
}

static void resolve_redirect_url(char *out, size_t outsz, const char *base_url, const char *location)
{
    if (!out || outsz == 0) {
        return;
    }
    out[0] = 0;
    if (!location || !location[0]) {
        return;
    }

    if (starts_with_http(location)) {
        snprintf(out, outsz, "%s", location);
        return;
    }

    if (location[0] == '/' && location[1] == '/') {
        const char *scheme_end = base_url ? strstr(base_url, "://") : NULL;
        if (scheme_end) {
            int scheme_len = (int)(scheme_end - base_url);
            snprintf(out, outsz, "%.*s:%s", scheme_len, base_url, location);
        }
        return;
    }

    char prefix[STREAM_URL_CAP];
    if (location[0] == '/') {
        if (url_origin_from(base_url, prefix, sizeof(prefix))) {
            snprintf(out, outsz, "%s%s", prefix, location);
            return;
        }
    } else if (url_directory_from(base_url, prefix, sizeof(prefix))) {
        snprintf(out, outsz, "%s%s", prefix, location);
        return;
    }

    build_url(out, outsz, location);
}

static Result http_request_full(HTTPC_RequestMethod method, const char *url, const char *body, bool include_token, HttpResponse *out)
{
    memset(out, 0, sizeof(*out));
    out->status = 0;
    out->result = 0;
    copy_safe(out->url, sizeof(out->url), url);
    if (!g_http_ready) {
        out->result = (Result)0xD9000003;
        return out->result;
    }

    char *active_url = NULL;
    char *redirect_url = NULL;
    active_url = strdup(url);
    if (!active_url) {
        return -1;
    }

    Result ret = 0;
    u32 status = 0;
    httpcContext context;
    memset(&context, 0, sizeof(context));
    bool context_open = false;

    for (int redirects = 0; redirects < HTTP_REDIRECT_LIMIT; redirects++) {
        copy_safe(out->url, sizeof(out->url), active_url);
        ret = httpcOpenContext(&context, method, active_url, method == HTTPC_METHOD_POST ? 0 : 1);
        if (R_FAILED(ret)) {
            break;
        }
        context_open = true;

        httpcSetSSLOpt(&context, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_DISABLED);
        httpcAddRequestHeaderField(&context, "User-Agent", "3dJelly/" APP_VERSION " Nintendo 3DS");
        httpcAddRequestHeaderField(&context, "Accept", "application/json, */*");
        httpcAddRequestHeaderField(&context, "Connection", "Close");

        char auth[384];
        auth_header(auth, sizeof(auth), include_token);
        httpcAddRequestHeaderField(&context, "Authorization", auth);
        httpcAddRequestHeaderField(&context, "X-Emby-Authorization", auth);
        if (include_token && g_cfg.token[0]) {
            httpcAddRequestHeaderField(&context, "X-Emby-Token", g_cfg.token);
        }

        if (body) {
            httpcAddRequestHeaderField(&context, "Content-Type", "application/json");
            ret = httpcAddPostDataRaw(&context, (u32 *)body, strlen(body));
            if (R_FAILED(ret)) {
                httpcCancelConnection(&context);
                httpcCloseContext(&context);
                context_open = false;
                break;
            }
        }

        ret = httpcBeginRequest(&context);
        if (R_FAILED(ret)) {
            httpcCancelConnection(&context);
            httpcCloseContext(&context);
            context_open = false;
            break;
        }

        ret = httpcGetResponseStatusCodeTimeout(&context, &status, HTTP_STATUS_TIMEOUT_NS);
        if (R_FAILED(ret)) {
            httpcCancelConnection(&context);
            httpcCloseContext(&context);
            context_open = false;
            break;
        }
        if (status == HTTP_STATUS_NONE) {
            ret = (Result)0xD9000001;
            httpcCancelConnection(&context);
            httpcCloseContext(&context);
            context_open = false;
            break;
        }

        if (http_status_is_redirect(status)) {
            if (redirects + 1 >= HTTP_REDIRECT_LIMIT) {
                ret = (Result)0xD9000004;
                httpcCancelConnection(&context);
                httpcCloseContext(&context);
                context_open = false;
                break;
            }
            if (!redirect_url) {
                redirect_url = (char *)malloc(1024);
            }
            if (!redirect_url) {
                ret = -1;
                httpcCancelConnection(&context);
                httpcCloseContext(&context);
                context_open = false;
                break;
            }
            memset(redirect_url, 0, 1024);
            ret = http_redirect_location(&context, redirect_url, 1024);
            httpcCancelConnection(&context);
            httpcCloseContext(&context);
            context_open = false;
            if (R_FAILED(ret) || !redirect_url[0]) {
                break;
            }
            char full[STREAM_URL_CAP];
            resolve_redirect_url(full, sizeof(full), active_url, redirect_url);
            if (!full[0]) {
                ret = -1;
                break;
            }
            free(active_url);
            active_url = strdup(full);
            if (!active_url) {
                ret = -1;
                break;
            }
            continue;
        }
        break;
    }

    out->status = status;

    if (R_SUCCEEDED(ret) && method != HTTPC_METHOD_HEAD && status != HTTP_STATUS_NONE) {
        char *buf = (char *)malloc(4096 + 1);
        if (!buf) {
            ret = -1;
        } else {
            size_t size = 0;
            u32 readsize = 0;
            do {
                if (size + 4096 + 1 > HTTP_CAP) {
                    ret = -3;
                    break;
                }
                ret = httpcDownloadData(&context, (u8 *)buf + size, 4096, &readsize);
                size += readsize;
                if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING) {
                    char *next = (char *)realloc(buf, size + 4096 + 1);
                    if (!next) {
                        ret = -1;
                        break;
                    }
                    buf = next;
                }
            } while (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING);

            if (R_SUCCEEDED(ret)) {
                buf[size] = 0;
                out->body = buf;
                out->size = size;
            } else {
                free(buf);
            }
        }
    }

    if (context_open) {
        if (R_FAILED(ret)) {
            httpcCancelConnection(&context);
        }
        httpcCloseContext(&context);
    }
    free(active_url);
    free(redirect_url);

    out->result = ret;
    return ret;
}

static void build_url(char *out, size_t outsz, const char *path)
{
    if (starts_with_http(path)) {
        snprintf(out, outsz, "%s", path);
    } else {
        snprintf(out, outsz, "%s%s%s", g_cfg.server, path[0] == '/' ? "" : "/", path);
    }
}

static Result api_get(const char *path, HttpResponse *out)
{
    char url[768];
    build_url(url, sizeof(url), path);
    return http_request_full(HTTPC_METHOD_GET, url, NULL, true, out);
}

static Result api_post(const char *path, const char *body, bool include_token, HttpResponse *out)
{
    char url[768];
    build_url(url, sizeof(url), path);
    return http_request_full(HTTPC_METHOD_POST, url, body, include_token, out);
}

static Result api_delete(const char *path, HttpResponse *out)
{
    char url[768];
    build_url(url, sizeof(url), path);
    return http_request_full(HTTPC_METHOD_DELETE, url, NULL, true, out);
}

static bool report_session_capabilities(void)
{
    if (g_session_capabilities_reported || !g_cfg.token[0] || !g_cfg.device_id[0]) {
        return g_session_capabilities_reported;
    }

    const char *body =
        "{"
        "\"PlayableMediaTypes\":[\"Video\"],"
        "\"SupportedCommands\":[\"PlayState\",\"DisplayMessage\",\"Mute\",\"Unmute\",\"ToggleMute\",\"SetVolume\",\"VolumeUp\",\"VolumeDown\"],"
        "\"SupportsMediaControl\":true,"
        "\"SupportsPersistentIdentifier\":true"
        "}";

    HttpResponse res;
    Result ret = api_post("/Sessions/Capabilities/Full", body, true, &res);
    bool ok = R_SUCCEEDED(ret) && res.status >= 200 && res.status < 300;
    bool unsupported = res.status == 404 || res.status == 405;
    free_response(&res);

    if (!ok && unsupported) {
        ret = api_post("/Sessions/Capabilities?playableMediaTypes=Video&supportedCommands=PlayState,DisplayMessage,Mute,Unmute,ToggleMute,SetVolume,VolumeUp,VolumeDown&supportsMediaControl=true&supportsPersistentIdentifier=true", NULL, true, &res);
        ok = R_SUCCEEDED(ret) && res.status >= 200 && res.status < 300;
        unsupported = res.status == 404 || res.status == 405;
        free_response(&res);
    }

    if (ok || unsupported) {
        g_session_capabilities_reported = true;
    }
    return ok;
}

static bool playback_report_ready(void)
{
    return g_cfg.token[0] && g_current.id[0] && g_play_session[0];
}

static int playback_report_thread_priority(void)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 4;
    if (prio < 0x18) {
        prio = 0x18;
    }
    if (prio > 0x3F) {
        prio = 0x3F;
    }
    return prio;
}

static void playback_report_worker_main(void *arg)
{
    (void)arg;

    for (;;) {
        PlaybackReportMessage msg;
        memset(&msg, 0, sizeof(msg));

        LightLock_Lock(&g_playback_report_lock);
        while (!g_playback_report_worker_stop && g_playback_report_queue_count <= 0) {
            CondVar_Wait(&g_playback_report_cv, &g_playback_report_lock);
        }
        if (g_playback_report_worker_stop && g_playback_report_queue_count <= 0) {
            LightLock_Unlock(&g_playback_report_lock);
            break;
        }

        msg = g_playback_report_queue[g_playback_report_queue_read];
        g_playback_report_queue_read = (g_playback_report_queue_read + 1) % PLAYBACK_REPORT_QUEUE_COUNT;
        g_playback_report_queue_count--;
        LightLock_Unlock(&g_playback_report_lock);

        if (msg.path[0] && msg.body[0]) {
            remote_http_post_json_quick(msg.path, msg.body, PLAYBACK_REPORT_TIMEOUT_MS);
        }
    }

    threadExit(0);
}

static bool playback_report_worker_start(void)
{
    if (!g_playback_report_sync_ready) {
        LightLock_Init(&g_playback_report_lock);
        CondVar_Init(&g_playback_report_cv);
        g_playback_report_sync_ready = true;
    }
    if (g_playback_report_worker_running) {
        return true;
    }

    g_playback_report_worker_stop = false;
    g_playback_report_queue_read = 0;
    g_playback_report_queue_write = 0;
    g_playback_report_queue_count = 0;
    g_playback_report_thread = threadCreate(playback_report_worker_main, NULL,
                                            PLAYBACK_REPORT_STACK_SIZE,
                                            playback_report_thread_priority(),
                                            -2, false);
    g_playback_report_worker_running = g_playback_report_thread != NULL;
    return g_playback_report_worker_running;
}

static void playback_report_worker_stop(bool drain)
{
    if (!g_playback_report_worker_running) {
        return;
    }

    if (!drain) {
        LightLock_Lock(&g_playback_report_lock);
        g_playback_report_queue_read = 0;
        g_playback_report_queue_write = 0;
        g_playback_report_queue_count = 0;
        LightLock_Unlock(&g_playback_report_lock);
    }

    LightLock_Lock(&g_playback_report_lock);
    g_playback_report_worker_stop = true;
    CondVar_Broadcast(&g_playback_report_cv);
    LightLock_Unlock(&g_playback_report_lock);

    threadJoin(g_playback_report_thread, U64_MAX);
    threadFree(g_playback_report_thread);
    g_playback_report_thread = NULL;
    g_playback_report_worker_running = false;
    g_playback_report_worker_stop = false;
}

static bool queue_playback_report(const char *path, const char *body)
{
    if (!path || !body || !path[0] || !body[0]) {
        return false;
    }
    if (!playback_report_worker_start()) {
        return false;
    }

    LightLock_Lock(&g_playback_report_lock);
    if (g_playback_report_queue_count >= PLAYBACK_REPORT_QUEUE_COUNT) {
        g_playback_report_queue_read = (g_playback_report_queue_read + 1) % PLAYBACK_REPORT_QUEUE_COUNT;
        g_playback_report_queue_count--;
    }

    PlaybackReportMessage *msg = &g_playback_report_queue[g_playback_report_queue_write];
    copy_safe(msg->path, sizeof(msg->path), path);
    copy_safe(msg->body, sizeof(msg->body), body);
    g_playback_report_queue_write = (g_playback_report_queue_write + 1) % PLAYBACK_REPORT_QUEUE_COUNT;
    g_playback_report_queue_count++;
    CondVar_Signal(&g_playback_report_cv);
    LightLock_Unlock(&g_playback_report_lock);
    return true;
}

static int playback_report_volume_level(int volume_percent, bool muted)
{
    if (muted || volume_percent < 0) {
        return 0;
    }
    if (volume_percent > 100) {
        return 100;
    }
    return volume_percent;
}

static void build_playback_state_body(char *out, size_t outsz, u64 position_ticks, bool paused, bool muted, int volume_percent)
{
    char item_id[128];
    char media_source_id[192];
    char play_session_id[128];
    json_escape(g_current.id, item_id, sizeof(item_id));
    json_escape(g_play_media_source_id, media_source_id, sizeof(media_source_id));
    json_escape(g_play_session, play_session_id, sizeof(play_session_id));

    snprintf(out, outsz,
             "{"
             "\"CanSeek\":true,"
             "\"ItemId\":\"%s\","
             "\"MediaSourceId\":\"%s\","
             "\"IsPaused\":%s,"
             "\"IsMuted\":%s,"
             "\"PositionTicks\":%llu,"
             "\"VolumeLevel\":%d,"
             "\"PlayMethod\":\"Transcode\","
             "\"PlaySessionId\":\"%s\","
             "\"RepeatMode\":\"RepeatNone\","
             "\"PlaybackOrder\":\"Default\","
             "\"NowPlayingQueue\":[]"
             "}",
             item_id,
             media_source_id,
             paused ? "true" : "false",
             muted ? "true" : "false",
             (unsigned long long)clamp_media_ticks(position_ticks),
             playback_report_volume_level(volume_percent, muted),
             play_session_id);
}

static void build_playback_stop_body(char *out, size_t outsz, u64 position_ticks)
{
    char item_id[128];
    char media_source_id[192];
    char play_session_id[128];
    json_escape(g_current.id, item_id, sizeof(item_id));
    json_escape(g_play_media_source_id, media_source_id, sizeof(media_source_id));
    json_escape(g_play_session, play_session_id, sizeof(play_session_id));

    snprintf(out, outsz,
             "{"
             "\"ItemId\":\"%s\","
             "\"MediaSourceId\":\"%s\","
             "\"PositionTicks\":%llu,"
             "\"PlaySessionId\":\"%s\","
             "\"Failed\":false,"
             "\"NowPlayingQueue\":[]"
             "}",
             item_id,
             media_source_id,
             (unsigned long long)clamp_media_ticks(position_ticks),
             play_session_id);
}

static bool report_playback_start(u64 position_ticks, bool paused, bool muted, int volume_percent)
{
    if (!playback_report_ready()) {
        return false;
    }

    report_session_capabilities();

    char body[1024];
    build_playback_state_body(body, sizeof(body), position_ticks, paused, muted, volume_percent);
    g_playback_report_active = true;
    g_playback_last_report_ms = osGetTime();
    return queue_playback_report("/Sessions/Playing", body);
}

static bool report_playback_progress(u64 position_ticks, bool paused, bool muted, int volume_percent)
{
    if (!playback_report_ready()) {
        return false;
    }

    char body[1024];
    build_playback_state_body(body, sizeof(body), position_ticks, paused, muted, volume_percent);
    g_playback_last_report_ms = osGetTime();
    return queue_playback_report("/Sessions/Playing/Progress", body);
}

static void report_playback_progress_periodic(u64 position_ticks, bool paused, bool muted, int volume_percent)
{
    if (!g_playback_report_active) {
        return;
    }

    u64 now_ms = osGetTime();
    if (g_playback_last_report_ms == 0 || now_ms - g_playback_last_report_ms >= PLAYBACK_REPORT_INTERVAL_MS) {
        report_playback_progress(position_ticks, paused, muted, volume_percent);
    }
}

static bool report_playback_stopped(u64 position_ticks)
{
    if (!g_playback_report_active) {
        return false;
    }
    g_playback_report_active = false;
    g_playback_last_report_ms = 0;

    if (!playback_report_ready()) {
        return false;
    }

    char body[1024];
    build_playback_stop_body(body, sizeof(body), position_ticks);
    if (app_system_closing()) {
        playback_report_worker_stop(false);
        return remote_http_post_json_quick("/Sessions/Playing/Stopped", body, 350);
    }
    bool queued = queue_playback_report("/Sessions/Playing/Stopped", body);
    playback_report_worker_stop(true);
    return queued;
}

static void stop_active_encoding(void)
{
    if (!g_cfg.device_id[0] || !g_play_session[0]) {
        return;
    }

    char device[160];
    char session[160];
    char path[384];
    url_encode(g_cfg.device_id, device, sizeof(device));
    url_encode(g_play_session, session, sizeof(session));
    snprintf(path, sizeof(path), "/Videos/ActiveEncodings?DeviceId=%s&PlaySessionId=%s", device, session);

    HttpResponse res;
    Result ret = api_delete(path, &res);
    if (R_FAILED(ret) && res.status != 404) {
        set_status(tr(TR_TRANSCODE_REFRESH_FAIL_FMT), (unsigned long)res.status, (unsigned long)ret);
    }
    free_response(&res);
}

static void set_http_failure(const char *prefix, const HttpResponse *res, Result ret)
{
    if (res->status == HTTP_STATUS_NONE) {
        set_status(tr(TR_HTTP_NO_RESPONSE_FMT), prefix, res->url);
    } else if (ret == (Result)HTTPC_RESULTCODE_TIMEDOUT) {
        set_status(tr(TR_HTTP_TIMEOUT_FMT), prefix);
    } else if (res->status == 401) {
        set_status(tr(TR_HTTP_401_FMT), prefix);
    } else {
        set_status(tr(TR_HTTP_FAIL_FMT), prefix, (unsigned long)res->status, (unsigned long)ret);
    }
}

static void format_http_failure(char *out, size_t outsz, const char *prefix, const HttpResponse *res, Result ret)
{
    if (res->status == HTTP_STATUS_NONE) {
        snprintf(out, outsz, tr(TR_HTTP_NO_RESPONSE_SHORT_FMT), prefix);
    } else if (ret == (Result)HTTPC_RESULTCODE_TIMEDOUT) {
        snprintf(out, outsz, tr(TR_HTTP_TIMEOUT_FMT), prefix);
    } else if (res->status == 401) {
        snprintf(out, outsz, tr(TR_HTTP_401_FMT), prefix);
    } else {
        snprintf(out, outsz, tr(TR_HTTP_FAIL_FMT), prefix, (unsigned long)res->status, (unsigned long)ret);
    }
}

static bool edit_text(const char *hint, char *buffer, size_t bufsz, bool password)
{
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, -1);
    swkbdSetInitialText(&kb, buffer);
    swkbdSetHintText(&kb, hint);
    swkbdSetValidation(&kb, password ? SWKBD_ANYTHING : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&kb, SWKBD_DARKEN_TOP_SCREEN | SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET | SWKBD_ALLOW_POWER);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, language_normalized(g_cfg.language) == LANG_ES ? "Cancelar" : "Cancel", false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
    if (password) {
        swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    }

    SwkbdButton button = swkbdInputText(&kb, buffer, bufsz);
    if (password) {
        trim_newline(buffer);
    } else {
        trim_edges(buffer);
    }
    return button == SWKBD_BUTTON_RIGHT;
}

static bool login_jellyfin(void)
{
    if (!starts_with_http(g_cfg.server) || !g_cfg.username[0]) {
        set_status("%s", tr(TR_STATUS_SET_SERVER_USER_FIRST));
        return false;
    }

    char user[192];
    char pass[192];
    json_escape(g_cfg.username, user, sizeof(user));
    json_escape(g_cfg.password, pass, sizeof(pass));

    char body[512];
    snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}", user, pass);

    HttpResponse res;
    set_status("%s", tr(TR_STATUS_LOGGING_IN));
    Result ret = api_post("/Users/AuthenticateByName", body, false, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        set_http_failure(tr(TR_PREFIX_LOGIN_FAILED), &res, ret);
        free_response(&res);
        return false;
    }

    const char *end = res.body + res.size;
    bool ok = json_get_string_range(res.body, end, "AccessToken", g_cfg.token, sizeof(g_cfg.token));
    const char *user_obj = NULL;
    const char *user_end = NULL;
    if (json_get_object_range(res.body, end, "User", &user_obj, &user_end)) {
        json_get_string_range(user_obj, user_end, "Id", g_cfg.user_id, sizeof(g_cfg.user_id));
    }

    free_response(&res);
    if (!ok || !g_cfg.token[0] || !g_cfg.user_id[0]) {
        set_status("%s", tr(TR_STATUS_LOGIN_MISSING));
        return false;
    }

    save_config();
    g_session_capabilities_reported = false;
    report_session_capabilities();
    set_status(tr(TR_STATUS_LOGGED_IN_FMT), g_cfg.username);
    return true;
}

static bool load_libraries(void)
{
    if (!g_cfg.token[0] || !g_cfg.user_id[0]) {
        return false;
    }
    report_session_capabilities();

    char path[256];
    snprintf(path, sizeof(path), "/Users/%s/Views?Fields=PrimaryImageAspectRatio", g_cfg.user_id);

    HttpResponse res;
    Result ret = api_get(path, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        set_http_failure(tr(TR_PREFIX_LOAD_LIBRARIES_FAILED), &res, ret);
        free_response(&res);
        return false;
    }

    g_library_count = 0;
    int raw_libraries = 0;
    parse_items(res.body, g_libraries, &g_library_count, MAX_LIBRARIES, false, &raw_libraries);
    free_response(&res);
    g_selected = 0;
    g_scroll = 0;
    g_stack_depth = 0;
    g_current_parent_id[0] = 0;
    copy_safe(g_screen_title, sizeof(g_screen_title), tr(TR_LIBRARIES));
    g_view = VIEW_LIBRARIES;
    set_status(tr(TR_STATUS_LOADED_LIBRARIES_FMT), g_library_count);
    return true;
}

static void build_items_page_path(char *path, size_t pathsz, const char *enc_parent, int start, int limit, bool user_endpoint)
{
    if (user_endpoint) {
        snprintf(path, pathsz,
                 "/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d&Recursive=false&EnableImages=false&EnableUserData=false&EnableTotalRecordCount=true&Fields=Path,MediaSources,ParentId&IsMissing=false&IsUnaired=false",
                 g_cfg.user_id, enc_parent, start, limit);
    } else {
        snprintf(path, pathsz,
                 "/Items?UserId=%s&ParentId=%s&StartIndex=%d&Limit=%d&Recursive=false&EnableImages=false&EnableUserData=false&EnableTotalRecordCount=true&Fields=Path,MediaSources,ParentId&IsMissing=false&IsUnaired=false",
                 g_cfg.user_id, enc_parent, start, limit);
    }
}

static bool load_items_for_parent(const char *parent_id, const char *title)
{
    char path[768];
    char enc_parent[160];
    url_encode(parent_id, enc_parent, sizeof(enc_parent));

    g_item_count = 0;
    int start = 0;
    int total = -1;
    bool user_endpoint = false;
    bool hit_cap = false;

    for (;;) {
        HttpResponse res;
        set_status(tr(TR_STATUS_LOADING_ITEMS_FMT), title && title[0] ? title : tr(TR_ITEMS_GENERIC), g_item_count);
        build_items_page_path(path, sizeof(path), enc_parent, start, ITEM_PAGE_SIZE, user_endpoint);
        Result ret = api_get(path, &res);
        if (!user_endpoint && start == 0 && parent_id[0] &&
            (R_FAILED(ret) || res.status == HTTP_STATUS_NONE || res.status >= 400)) {
            free_response(&res);
            user_endpoint = true;
            build_items_page_path(path, sizeof(path), enc_parent, start, ITEM_PAGE_SIZE, user_endpoint);
            ret = api_get(path, &res);
        }
        if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
            set_http_failure(tr(TR_PREFIX_LOAD_ITEMS_FAILED), &res, ret);
            free_response(&res);
            return false;
        }

        int raw_count = 0;
        if (!parse_items(res.body, g_items, &g_item_count, MAX_ITEMS, true, &raw_count)) {
            set_status("%s", tr(TR_STATUS_PARSE_ITEMS_FAIL));
            free_response(&res);
            return false;
        }
        int page_total = parse_total_record_count(res.body);
        if (page_total >= 0) {
            total = page_total;
        }
        free_response(&res);

        if (raw_count <= 0) {
            break;
        }

        start += raw_count;
        if (g_item_count >= MAX_ITEMS) {
            hit_cap = total < 0 || start < total || raw_count >= ITEM_PAGE_SIZE;
            break;
        }
        if (total >= 0) {
            if (start >= total) {
                break;
            }
        } else if (raw_count < ITEM_PAGE_SIZE) {
            break;
        }
    }

    g_selected = 0;
    g_scroll = 0;
    copy_safe(g_current_parent_id, sizeof(g_current_parent_id), parent_id);
    copy_safe(g_screen_title, sizeof(g_screen_title), title && title[0] ? title : tr(TR_ITEMS_GENERIC));
    g_view = VIEW_ITEMS;
    if (hit_cap) {
        set_status(tr(TR_STATUS_FIRST_ENTRIES_FMT), g_screen_title, g_item_count);
    } else {
        set_status(tr(TR_STATUS_ENTRIES_FMT), g_screen_title, g_item_count);
    }
    return true;
}

static bool is_playable(const MediaItem *item)
{
    if (media_item_is_unowned_placeholder(item)) {
        return false;
    }
    if (strcmp(item->type, "Audio") == 0) {
        return false;
    }
    return strcmp(item->type, "Movie") == 0 ||
           strcmp(item->type, "Episode") == 0 ||
           strcmp(item->type, "Video") == 0 ||
           strcmp(item->type, "MusicVideo") == 0 ||
           (!item->is_folder && item->id[0]);
}

static bool media_item_is_episode(const MediaItem *item)
{
    return item && strcmp(item->type, "Episode") == 0 && is_playable(item);
}

static bool media_items_same_series(const MediaItem *a, const MediaItem *b)
{
    if (!a || !b || !a->series_id[0] || !b->series_id[0]) {
        return false;
    }
    return strcmp(a->series_id, b->series_id) == 0;
}

static bool load_current_episode_details_if_needed(void)
{
    if (!media_item_is_episode(&g_current) || g_current.series_id[0]) {
        return g_current.series_id[0] != 0;
    }

    char path[256];
    char enc_id[128];
    url_encode(g_current.id, enc_id, sizeof(enc_id));
    snprintf(path, sizeof(path), "/Users/%s/Items/%s", g_cfg.user_id, enc_id);

    HttpResponse res;
    Result ret = api_get(path, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        free_response(&res);
        return false;
    }

    MediaItem detail;
    parse_media_item_object(res.body, res.body + res.size, &detail);
    free_response(&res);
    if (!detail.id[0] || strcmp(detail.id, g_current.id) != 0 || !detail.series_id[0]) {
        return false;
    }

    g_current = detail;
    return true;
}

static bool find_next_episode_in_loaded_items(MediaItem *out)
{
    if (!out || !media_item_is_episode(&g_current)) {
        return false;
    }

    int current_index = -1;
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].id[0] && strcmp(g_items[i].id, g_current.id) == 0) {
            current_index = i;
            break;
        }
    }
    if (current_index < 0) {
        return false;
    }

    for (int i = current_index + 1; i < g_item_count; i++) {
        if (!media_item_is_episode(&g_items[i])) {
            continue;
        }
        if (g_current.series_id[0] && (!g_items[i].series_id[0] || !media_items_same_series(&g_current, &g_items[i]))) {
            continue;
        }
        *out = g_items[i];
        return true;
    }
    return false;
}

static bool query_next_episode_from_server(MediaItem *out)
{
    if (!out || !media_item_is_episode(&g_current) || !load_current_episode_details_if_needed()) {
        return false;
    }

    char series[128];
    char current[128];
    char path[512];
    url_encode(g_current.series_id, series, sizeof(series));
    url_encode(g_current.id, current, sizeof(current));
    snprintf(path, sizeof(path),
             "/Shows/%s/Episodes?userId=%s&startItemId=%s&limit=4&fields=Path,MediaSources,ParentId&isMissing=false&enableImages=false&enableUserData=false",
             series, g_cfg.user_id, current);

    HttpResponse res;
    Result ret = api_get(path, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        free_response(&res);
        return false;
    }

    MediaItem candidates[4];
    int count = 0;
    int raw = 0;
    bool parsed = parse_items(res.body, candidates, &count, 4, true, &raw);
    free_response(&res);
    if (!parsed) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (!media_item_is_episode(&candidates[i]) || strcmp(candidates[i].id, g_current.id) == 0) {
            continue;
        }
        if (candidates[i].series_id[0] && !media_items_same_series(&g_current, &candidates[i])) {
            continue;
        }
        *out = candidates[i];
        return true;
    }
    return false;
}

static void prepare_autoplay_next_episode(void)
{
    memset(&g_autoplay_next_item, 0, sizeof(g_autoplay_next_item));
    g_autoplay_next_ready = true;
    g_autoplay_next_available = false;
    g_autoplay_cancelled = false;

    if (!media_item_is_episode(&g_current)) {
        return;
    }
    if (find_next_episode_in_loaded_items(&g_autoplay_next_item) ||
        query_next_episode_from_server(&g_autoplay_next_item)) {
        g_autoplay_next_available = true;
    }
}

static void push_nav(const char *parent_id, const char *title)
{
    if (g_stack_depth >= MAX_STACK) {
        return;
    }
    copy_safe(g_stack[g_stack_depth].parent_id, sizeof(g_stack[g_stack_depth].parent_id), parent_id);
    copy_safe(g_stack[g_stack_depth].title, sizeof(g_stack[g_stack_depth].title), title);
    g_stack[g_stack_depth].selected = g_selected;
    g_stack[g_stack_depth].scroll = g_scroll;
    g_stack_depth++;
}

static void pop_nav(void)
{
    if (g_stack_depth <= 0) {
        load_libraries();
        return;
    }

    NavFrame f = g_stack[--g_stack_depth];
    if (f.parent_id[0]) {
        load_items_for_parent(f.parent_id, f.title);
    } else {
        load_libraries();
    }
    g_selected = f.selected;
    g_scroll = f.scroll;
}

static void append_query(char *url, size_t urlsz, const char *query)
{
    if (strlen(url) + strlen(query) + 2 >= urlsz) {
        return;
    }
    strcat(url, strchr(url, '?') ? "&" : "?");
    strcat(url, query);
}

static void build_fallback_stream_url(const MediaItem *item, char *out, size_t outsz)
{
    QualityProfile q = quality_profile();
    char id[160];
    char token[256];
    char dev[160];
    char media_source[192];
    char session[160];
    char media_q[224] = "";
    char session_q[192] = "";
    url_encode(item->id, id, sizeof(id));
    url_encode(g_cfg.token, token, sizeof(token));
    url_encode(g_cfg.device_id, dev, sizeof(dev));
    url_encode(g_play_media_source_id, media_source, sizeof(media_source));
    url_encode(g_play_session, session, sizeof(session));
    if (media_source[0]) {
        snprintf(media_q, sizeof(media_q), "&MediaSourceId=%s", media_source);
    }
    if (session[0]) {
        snprintf(session_q, sizeof(session_q), "&PlaySessionId=%s", session);
    }
    snprintf(out, outsz,
             "%s/Videos/%s/stream?Container=ts&DeviceId=%s%s%s&VideoCodec=h264&AudioCodec=aac&VideoBitrate=%d&AudioBitrate=%d&MaxWidth=%d&MaxHeight=%d&MaxFramerate=%d&Static=false&EnableAutoStreamCopy=false&TranscodingMaxAudioChannels=2&RequireAvc=true&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&SubtitleMethod=Encode&Context=Streaming&ApiKey=%s",
             g_cfg.server, id, dev, media_q, session_q, q.video_bitrate, q.audio_bitrate, q.width, q.height, q.max_fps, token);
}

static void build_mjpeg_stream_url(const MediaItem *item, char *out, size_t outsz, bool avi_container, u64 start_time_ticks)
{
    QualityProfile q = quality_profile();
    int fps = mjpeg_target_fps();
    int bitrate = mjpeg_target_bitrate();
    int audio_sample_rate = mjpeg_target_audio_sample_rate();
    char id[160];
    char token[256];
    char dev[160];
    char media_source[192];
    char session[160];
    char media_q[224] = "";
    char session_q[192] = "";
    char start_q[96] = "";
    char refresh_q[96] = "";
    url_encode(item->id, id, sizeof(id));
    url_encode(g_cfg.token, token, sizeof(token));
    url_encode(g_cfg.device_id, dev, sizeof(dev));
    url_encode(g_play_media_source_id, media_source, sizeof(media_source));
    url_encode(g_play_session, session, sizeof(session));
    if (media_source[0]) {
        snprintf(media_q, sizeof(media_q), "&MediaSourceId=%s", media_source);
    }
    if (session[0]) {
        snprintf(session_q, sizeof(session_q), "&PlaySessionId=%s", session);
    }
    if (start_time_ticks > 0) {
        snprintf(start_q, sizeof(start_q), "&StartTimeTicks=%llu", (unsigned long long)start_time_ticks);
    }
    snprintf(refresh_q, sizeof(refresh_q), "&3dJellyQuality=%d&3dJellySwitch=%lu", g_cfg.quality, (unsigned long)g_stream_switch_serial);
    if (avi_container) {
        snprintf(out, outsz,
                 "%s/Videos/%s/stream?Container=avi&DeviceId=%s%s%s%s%s&VideoCodec=mjpeg&AudioCodec=pcm_s16le&VideoBitRate=%d&AudioBitRate=%d&AudioSampleRate=%d&AudioChannels=1&MaxAudioChannels=1&TranscodingMaxAudioChannels=1&Width=%d&Height=%d&MaxWidth=%d&MaxHeight=%d&Framerate=%d&MaxFramerate=%d&Static=false&EnableAutoStreamCopy=false&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&SubtitleMethod=Encode&Context=Streaming&TranscodeReasons=ContainerNotSupported,VideoCodecNotSupported,AudioCodecNotSupported&ApiKey=%s",
                 g_cfg.server, id, dev, media_q, session_q, start_q, refresh_q, bitrate, audio_sample_rate * AUDIO_CHANNELS * 16, audio_sample_rate, q.width, q.height, q.width, q.height, fps, fps, token);
    } else {
        snprintf(out, outsz,
                 "%s/Videos/%s/stream?Container=mjpeg&DeviceId=%s%s%s%s%s&VideoCodec=mjpeg&VideoBitRate=%d&Width=%d&Height=%d&MaxWidth=%d&MaxHeight=%d&Framerate=%d&MaxFramerate=%d&Static=false&EnableAutoStreamCopy=false&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&SubtitleMethod=Encode&Context=Streaming&TranscodeReasons=ContainerNotSupported,VideoCodecNotSupported&ApiKey=%s",
                 g_cfg.server, id, dev, media_q, session_q, start_q, refresh_q, bitrate, q.width, q.height, q.width, q.height, fps, fps, token);
    }
}

static void build_mpeg1_stream_url(const MediaItem *item, char *out, size_t outsz, u64 start_time_ticks, bool mpg_alias)
{
    QualityProfile q = quality_profile();
    int fps = mjpeg_target_fps();
    char id[160];
    char token[256];
    char dev[160];
    char media_source[192];
    char session[160];
    char media_q[224] = "";
    char session_q[192] = "";
    char start_q[96] = "";
    char refresh_q[96] = "";
    url_encode(item->id, id, sizeof(id));
    url_encode(g_cfg.token, token, sizeof(token));
    url_encode(g_cfg.device_id, dev, sizeof(dev));
    url_encode(g_play_media_source_id, media_source, sizeof(media_source));
    url_encode(g_play_session, session, sizeof(session));
    if (media_source[0]) {
        snprintf(media_q, sizeof(media_q), "&MediaSourceId=%s", media_source);
    }
    if (session[0]) {
        snprintf(session_q, sizeof(session_q), "&PlaySessionId=%s", session);
    }
    if (start_time_ticks > 0) {
        snprintf(start_q, sizeof(start_q), "&StartTimeTicks=%llu", (unsigned long long)start_time_ticks);
    }
    snprintf(refresh_q, sizeof(refresh_q), "&3dJellyQuality=%d&3dJellySwitch=%lu", g_cfg.quality, (unsigned long)g_stream_switch_serial);

    snprintf(out, outsz,
             "%s/Videos/%s/stream?Container=%s&DeviceId=%s%s%s%s%s&VideoCodec=mpeg1video&AudioCodec=mp2&VideoBitRate=%d&AudioBitRate=%d&AudioSampleRate=44100&AudioChannels=1&MaxAudioChannels=1&TranscodingMaxAudioChannels=1&Width=%d&Height=%d&MaxWidth=%d&MaxHeight=%d&Framerate=%d&MaxFramerate=%d&Static=false&EnableAutoStreamCopy=false&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&SubtitleMethod=Encode&Context=Streaming&TranscodeReasons=ContainerNotSupported,VideoCodecNotSupported,AudioCodecNotSupported&ApiKey=%s",
             g_cfg.server, id, mpg_alias ? "mpg" : "mpeg", dev, media_q, session_q, start_q, refresh_q,
             q.video_bitrate, q.audio_bitrate, q.width, q.height, q.width, q.height, fps, fps, token);
}

static void normalize_transcode_url(char *url, size_t urlsz)
{
    if (!url[0]) {
        return;
    }

    char full[STREAM_URL_CAP];
    if (starts_with_http(url)) {
        copy_safe(full, sizeof(full), url);
    } else {
        snprintf(full, sizeof(full), "%s%s%s", g_cfg.server, url[0] == '/' ? "" : "/", url);
    }
    copy_safe(url, urlsz, full);

    QualityProfile q = quality_profile();
    char query[256];
    if (!strstr(url, "MaxWidth=") && !strstr(url, "maxWidth=")) {
        snprintf(query, sizeof(query), "MaxWidth=%d&MaxHeight=%d", q.width, q.height);
        append_query(url, urlsz, query);
    }
    if (!strstr(url, "VideoBitrate=") && !strstr(url, "videoBitRate=")) {
        snprintf(query, sizeof(query), "VideoBitrate=%d&AudioBitrate=%d", q.video_bitrate, q.audio_bitrate);
        append_query(url, urlsz, query);
    }
    if (!strstr(url, "MaxFramerate=") && !strstr(url, "maxFramerate=")) {
        snprintf(query, sizeof(query), "MaxFramerate=%d", q.max_fps);
        append_query(url, urlsz, query);
    }
    if (!strstr(url, "RequireAvc=") && !strstr(url, "requireAvc=")) {
        append_query(url, urlsz, "RequireAvc=true");
    }
    if (g_play_media_source_id[0] && !strstr(url, "MediaSourceId=") && !strstr(url, "mediaSourceId=")) {
        char enc[192];
        char query[224];
        url_encode(g_play_media_source_id, enc, sizeof(enc));
        snprintf(query, sizeof(query), "MediaSourceId=%s", enc);
        append_query(url, urlsz, query);
    }
    if (g_play_session[0] && !strstr(url, "PlaySessionId=") && !strstr(url, "playSessionId=")) {
        char enc[160];
        char query[192];
        url_encode(g_play_session, enc, sizeof(enc));
        snprintf(query, sizeof(query), "PlaySessionId=%s", enc);
        append_query(url, urlsz, query);
    }
}

static void build_playback_body(char *out, size_t outsz, u64 start_time_ticks)
{
    QualityProfile q = quality_profile();
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    start_time_ticks = clamp_media_ticks(start_time_ticks);
    snprintf(out, outsz,
        "{"
        "\"UserId\":\"%s\","
        "\"StartTimeTicks\":%llu,"
        "\"MaxStreamingBitrate\":%d,"
        "\"MaxAudioChannels\":2,"
        "\"SubtitleStreamIndex\":-1,"
        "\"EnableDirectPlay\":false,"
        "\"EnableDirectStream\":false,"
        "\"EnableTranscoding\":true,"
        "\"AllowVideoStreamCopy\":false,"
        "\"AllowAudioStreamCopy\":false,"
        "\"AlwaysBurnInSubtitleWhenTranscoding\":true,"
        "\"DeviceProfile\":{"
            "\"Name\":\"3dJelly %s\","
            "\"MaxStreamingBitrate\":%d,"
            "\"MaxStaticBitrate\":%d,"
            "\"MusicStreamingTranscodingBitrate\":96000,"
            "\"DirectPlayProfiles\":[],"
            "\"TranscodingProfiles\":[{"
                "\"Container\":\"ts\","
                "\"Type\":\"Video\","
                "\"VideoCodec\":\"h264\","
                "\"AudioCodec\":\"aac\","
                "\"Protocol\":\"http\","
                "\"Context\":\"Streaming\","
                "\"MaxAudioChannels\":\"2\","
                "\"MinSegments\":1,"
                "\"EstimateContentLength\":false"
            "}],"
            "\"ContainerProfiles\":[],"
            "\"CodecProfiles\":["
                "{\"Type\":\"Video\",\"Conditions\":["
                    "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\",\"Value\":\"%d\",\"IsRequired\":true},"
                    "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\",\"Value\":\"%d\",\"IsRequired\":true},"
                    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitrate\",\"Value\":\"%d\",\"IsRequired\":true},"
                    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoFramerate\",\"Value\":\"%d\",\"IsRequired\":true}"
                "]},"
                "{\"Type\":\"Audio\",\"Conditions\":["
                    "{\"Condition\":\"LessThanEqual\",\"Property\":\"AudioChannels\",\"Value\":\"2\",\"IsRequired\":true},"
                    "{\"Condition\":\"LessThanEqual\",\"Property\":\"AudioBitrate\",\"Value\":\"%d\",\"IsRequired\":true}"
                "]}"
            "],"
            "\"SubtitleProfiles\":[]"
        "}"
        "}",
        g_cfg.user_id, (unsigned long long)start_time_ticks, q.video_bitrate + q.audio_bitrate, quality_label,
        q.video_bitrate + q.audio_bitrate, q.video_bitrate + q.audio_bitrate,
        q.width, q.height, q.video_bitrate, q.max_fps, q.audio_bitrate);
}

static void set_play_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_play_status, sizeof(g_play_status), fmt, ap);
    va_end(ap);
    set_status("%s", g_play_status);
}

static bool request_playback_info(u64 start_time_ticks)
{
    char path[256];
    char body[4096];
    char enc_id[128];
    url_encode(g_current.id, enc_id, sizeof(enc_id));
    snprintf(path, sizeof(path), "/Items/%s/PlaybackInfo?UserId=%s", enc_id, g_cfg.user_id);
    build_playback_body(body, sizeof(body), start_time_ticks);

    g_play_url[0] = 0;
    g_play_method[0] = 0;
    g_play_media_source_id[0] = 0;
    g_play_session[0] = 0;

    HttpResponse res;
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    snprintf(g_play_status, sizeof(g_play_status), tr(TR_PLAYBACK_REQUESTING_FMT), quality_label);
    set_status("%s", g_play_status);
    Result ret = api_post(path, body, true, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        format_http_failure(g_play_status, sizeof(g_play_status), tr(TR_PREFIX_PLAYBACK_INFO_FAILED), &res, ret);
        set_status("%s", g_play_status);
        free_response(&res);
        return false;
    }

    const char *end = res.body + res.size;
    json_get_string_range(res.body, end, "PlaySessionId", g_play_session, sizeof(g_play_session));

    const char *arr = find_key_range(res.body, end, "MediaSources");
    const char *src_start = NULL;
    const char *src_end = NULL;
    if (arr) {
        arr = skip_ws(arr, end);
        if (arr < end && *arr == '[') {
            json_object_range_after(arr + 1, end, &src_start, &src_end);
        }
    }

    bool supports_transcoding = false;
    bool supports_direct = false;
    if (src_start && src_end) {
        json_get_string_range(src_start, src_end, "Id", g_play_media_source_id, sizeof(g_play_media_source_id));
        json_get_string_range(src_start, src_end, "TranscodingUrl", g_play_url, sizeof(g_play_url));
        json_get_string_range(src_start, src_end, "TranscodingContainer", g_play_method, sizeof(g_play_method));
        json_get_bool_range(src_start, src_end, "SupportsTranscoding", &supports_transcoding);
        json_get_bool_range(src_start, src_end, "SupportsDirectPlay", &supports_direct);
    }

    if (!g_play_url[0]) {
        build_fallback_stream_url(&g_current, g_play_url, sizeof(g_play_url));
        copy_safe(g_play_method, sizeof(g_play_method), "manual-ts");
    } else {
        normalize_transcode_url(g_play_url, sizeof(g_play_url));
    }

    free_response(&res);

    snprintf(g_play_status, sizeof(g_play_status),
             tr(TR_PLAYBACK_SESSION_OK_FMT),
             quality_label,
             supports_transcoding ? tr(TR_YES) : tr(TR_NO),
             supports_direct ? tr(TR_YES) : tr(TR_NO));
    set_status("%s", g_play_status);
    return true;
}

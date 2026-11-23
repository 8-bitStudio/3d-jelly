/* Audio and subtitle track selection for video playback.
 *
 * The tracks come from the MediaStreams of the PlaybackInfo media source. A new
 * item starts on Jellyfin's own defaults (which follow the user's language
 * settings), or on the language the user last picked by hand. The choice then
 * sticks for that item across stream restarts (seek, quality, track change).
 *
 * Text subtitles are downloaded once as SRT and drawn by the 3DS on top of the
 * video at full screen resolution. Only image subtitles (PGS, DVD) can't be
 * drawn that way; those are burned into the video by the server. */

#define MEDIA_TRACK_MAX 32
#define MEDIA_TRACK_TEXT_CAP 96
#define MEDIA_TRACK_LANG_CAP 16

typedef struct {
    int index;                        /* Jellyfin MediaStream index */
    bool text;                        /* text subtitle: drawn on the 3DS */
    bool forced;
    char lang[MEDIA_TRACK_LANG_CAP];
    char title[MEDIA_TRACK_TEXT_CAP];  /* first line in the picker */
    char detail[MEDIA_TRACK_TEXT_CAP]; /* second, smaller line */
} MediaTrack;

static MediaTrack g_audio_tracks[MEDIA_TRACK_MAX];
static int g_audio_track_count;
static MediaTrack g_sub_tracks[MEDIA_TRACK_MAX];
static int g_sub_track_count;
static char g_tracks_item_id[80];
static int g_default_audio_index = -1; /* what Jellyfin plays when we don't ask */
static int g_sel_audio_index = -1;     /* -1: let Jellyfin pick */
static int g_sel_sub_index = -1;       /* -1: subtitles off */

/* Carried to the next item (e.g. the next episode) once the user picks by hand. */
static bool g_pref_audio_set;
static char g_pref_audio_lang[MEDIA_TRACK_LANG_CAP];
static bool g_pref_sub_set;
static bool g_pref_sub_off;
static char g_pref_sub_lang[MEDIA_TRACK_LANG_CAP];

static Result api_get(const char *path, HttpResponse *out);
static void free_response(HttpResponse *res);
static void subtitles_release(void);

static const MediaTrack *media_track_find(const MediaTrack *list, int count, int index)
{
    if (index < 0) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        if (list[i].index == index) {
            return &list[i];
        }
    }
    return NULL;
}

static bool tracks_for_current_item(void)
{
    return g_tracks_item_id[0] && strcmp(g_tracks_item_id, g_current.id) == 0;
}

static int tracks_effective_audio_index(void)
{
    return g_sel_audio_index >= 0 ? g_sel_audio_index : g_default_audio_index;
}

static const MediaTrack *tracks_selected_sub(void)
{
    return tracks_for_current_item() ? media_track_find(g_sub_tracks, g_sub_track_count, g_sel_sub_index) : NULL;
}

/* Audio index to request from the server, or -1 for the server default. */
static int tracks_request_audio_index(void)
{
    return tracks_for_current_item() ? g_sel_audio_index : -1;
}

/* Subtitle index the server must burn in (image subtitles only), or -1. */
static int tracks_request_burn_in_index(void)
{
    const MediaTrack *sub = tracks_selected_sub();
    return sub && !sub->text ? sub->index : -1;
}

static int tracks_selected_text_sub_index(void)
{
    const MediaTrack *sub = tracks_selected_sub();
    return sub && sub->text ? sub->index : -1;
}

/* Appends "&AudioStreamIndex=..&SubtitleStreamIndex=.." for the current choice. */
static void tracks_stream_query(char *out, size_t outsz)
{
    if (!out || !outsz) {
        return;
    }
    out[0] = 0;
    int audio = tracks_request_audio_index();
    int burn = tracks_request_burn_in_index();
    size_t used = 0;
    if (audio >= 0) {
        int n = snprintf(out, outsz, "&AudioStreamIndex=%d", audio);
        used = n > 0 ? (size_t)n : 0;
    }
    if (burn >= 0 && used < outsz) {
        snprintf(out + used, outsz - used, "&SubtitleStreamIndex=%d", burn);
    }
}

static bool tracks_lang_equal(const char *a, const char *b)
{
    return a && b && a[0] && b[0] && strcasecmp(a, b) == 0;
}

/* Splits Jellyfin's DisplayTitle ("Original audio - Japanese - AAC - Stereo")
 * into a short first line and the rest as detail. */
static void tracks_split_display_title(const char *display, MediaTrack *track)
{
    track->title[0] = 0;
    track->detail[0] = 0;
    if (!display || !display[0]) {
        return;
    }
    const char *sep = strstr(display, " - ");
    if (!sep) {
        copy_safe(track->title, sizeof(track->title), display);
        return;
    }
    size_t n = (size_t)(sep - display);
    if (n >= sizeof(track->title)) {
        n = sizeof(track->title) - 1;
    }
    memcpy(track->title, display, n);
    track->title[n] = 0;
    copy_safe(track->detail, sizeof(track->detail), sep + 3);
}

static void tracks_parse_stream(const char *os, const char *oe)
{
    char type[24];
    if (!json_get_string_range(os, oe, "Type", type, sizeof(type))) {
        return;
    }
    bool audio = strcmp(type, "Audio") == 0;
    bool subtitle = strcmp(type, "Subtitle") == 0;
    if (!audio && !subtitle) {
        return;
    }
    MediaTrack *list = audio ? g_audio_tracks : g_sub_tracks;
    int *count = audio ? &g_audio_track_count : &g_sub_track_count;
    if (*count >= MEDIA_TRACK_MAX) {
        return;
    }

    int index = -1;
    if (!json_get_int_range(os, oe, "Index", &index) || index < 0) {
        return;
    }

    MediaTrack *track = &list[*count];
    memset(track, 0, sizeof(*track));
    track->index = index;
    json_get_string_range(os, oe, "Language", track->lang, sizeof(track->lang));
    json_get_bool_range(os, oe, "IsForced", &track->forced);
    if (subtitle) {
        track->text = false;
        json_get_bool_range(os, oe, "IsTextSubtitleStream", &track->text);
    }

    char display[MEDIA_TRACK_TEXT_CAP * 2];
    if (!json_get_string_range(os, oe, "DisplayTitle", display, sizeof(display)) || !display[0]) {
        if (!json_get_string_range(os, oe, "Title", display, sizeof(display)) || !display[0]) {
            copy_safe(display, sizeof(display), track->lang);
        }
    }
    tracks_split_display_title(display, track);
    if (!track->title[0]) {
        snprintf(track->title, sizeof(track->title), tr(TR_TRACKS_TRACK_FMT), *count + 1);
    }
    if (subtitle && !track->text) {
        size_t len = strlen(track->detail);
        snprintf(track->detail + len, sizeof(track->detail) - len, "%s%s", len ? " - " : "", tr(TR_TRACKS_BURNED_IN));
    }
    (*count)++;
}

static int tracks_pick_audio_by_lang(const char *lang)
{
    for (int i = 0; i < g_audio_track_count; i++) {
        if (tracks_lang_equal(g_audio_tracks[i].lang, lang)) {
            return g_audio_tracks[i].index;
        }
    }
    return -1;
}

static int tracks_pick_text_sub_by_lang(const char *lang)
{
    int forced_match = -1;
    for (int i = 0; i < g_sub_track_count; i++) {
        const MediaTrack *t = &g_sub_tracks[i];
        if (!t->text || !tracks_lang_equal(t->lang, lang)) {
            continue;
        }
        if (!t->forced) {
            return t->index;
        }
        if (forced_match < 0) {
            forced_match = t->index;
        }
    }
    return forced_match;
}

/* Called from request_playback_info with the first MediaSource object. */
static void tracks_update_from_source(const char *src_start, const char *src_end)
{
    g_audio_track_count = 0;
    g_sub_track_count = 0;
    if (!src_start || !src_end) {
        return;
    }

    int default_audio = -1;
    int default_sub = -1;
    json_get_int_range(src_start, src_end, "DefaultAudioStreamIndex", &default_audio);
    json_get_int_range(src_start, src_end, "DefaultSubtitleStreamIndex", &default_sub);

    const char *arr = find_key_range(src_start, src_end, "MediaStreams");
    if (arr && arr < src_end && *arr == '[') {
        const char *p = arr + 1;
        while (p < src_end) {
            p = skip_ws(p, src_end);
            if (p >= src_end || *p == ']') {
                break;
            }
            if (*p == ',') {
                p++;
                continue;
            }
            const char *os = NULL;
            const char *oe = NULL;
            if (!json_object_range_after(p, src_end, &os, &oe)) {
                break;
            }
            tracks_parse_stream(os, oe);
            p = oe;
        }
    }

    bool same_item = tracks_for_current_item();
    if (!same_item) {
        subtitles_release();
        copy_safe(g_tracks_item_id, sizeof(g_tracks_item_id), g_current.id);
        g_default_audio_index = media_track_find(g_audio_tracks, g_audio_track_count, default_audio) ? default_audio : -1;
        g_sel_audio_index = -1;
        g_sel_sub_index = -1;

        if (g_pref_audio_set) {
            int idx = tracks_pick_audio_by_lang(g_pref_audio_lang);
            if (idx >= 0 && idx != g_default_audio_index) {
                g_sel_audio_index = idx;
            }
        }

        if (g_pref_sub_set) {
            g_sel_sub_index = g_pref_sub_off ? -1 : tracks_pick_text_sub_by_lang(g_pref_sub_lang);
        } else {
            /* Jellyfin's default only applies when the 3DS can draw it. */
            const MediaTrack *sub = media_track_find(g_sub_tracks, g_sub_track_count, default_sub);
            g_sel_sub_index = sub && sub->text ? default_sub : -1;
        }
    } else {
        if (g_sel_audio_index < 0 && media_track_find(g_audio_tracks, g_audio_track_count, default_audio)) {
            g_default_audio_index = default_audio;
        }
        if (g_sel_audio_index >= 0 && !media_track_find(g_audio_tracks, g_audio_track_count, g_sel_audio_index)) {
            g_sel_audio_index = -1;
        }
        if (g_sel_sub_index >= 0 && !media_track_find(g_sub_tracks, g_sub_track_count, g_sel_sub_index)) {
            g_sel_sub_index = -1;
        }
    }
}

/* Forget the per-item choice so the next playback starts from defaults. The
 * language preference set by hand is kept for the rest of the session. */
static void tracks_reset_item(void)
{
    g_tracks_item_id[0] = 0;
    g_sel_audio_index = -1;
    g_sel_sub_index = -1;
    g_default_audio_index = -1;
}

/* ---- Subtitle download and parsing ----------------------------------- */

typedef struct {
    u32 start_ms;
    u32 end_ms;
    u32 text_off;
} SubtitleCue;

typedef struct {
    SubtitleCue *cues;
    int count;
    char *text;
    int stream_index;
    char item_id[80];
} SubtitleData;

typedef enum {
    SUB_STATE_NONE,
    SUB_STATE_LOADING,
    SUB_STATE_READY,
    SUB_STATE_FAILED
} SubtitleState;

static SubtitleData g_subs = { NULL, 0, NULL, -1, "" }; /* main thread only */
static SubtitleData g_sub_job_result;
static Thread g_sub_thread;
static bool g_sub_job_running;
static volatile bool g_sub_job_done;
static int g_sub_job_index = -1;
static char g_sub_job_path[512];
static int g_sub_failed_index = -1;
static char g_sub_failed_item[80];
static u32 g_sub_generation; /* bumps whenever g_subs changes */

static void subtitle_data_free(SubtitleData *data)
{
    free(data->cues);
    free(data->text);
    data->cues = NULL;
    data->text = NULL;
    data->count = 0;
    data->stream_index = -1;
    data->item_id[0] = 0;
}

static bool srt_read_uint(const char **pp, const char *end, u32 *out, int *digits)
{
    const char *p = *pp;
    u32 v = 0;
    int n = 0;
    while (p < end && *p >= '0' && *p <= '9' && n < 9) {
        v = v * 10u + (u32)(*p - '0');
        p++;
        n++;
    }
    if (!n) {
        return false;
    }
    *pp = p;
    *out = v;
    if (digits) {
        *digits = n;
    }
    return true;
}

/* Parses "HH:MM:SS,mmm" (also "MM:SS.mmm") into milliseconds. */
static bool srt_parse_time(const char **pp, const char *end, u32 *ms_out)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    u32 parts[3] = {0, 0, 0};
    int count = 0;
    for (;;) {
        u32 v = 0;
        if (!srt_read_uint(&p, end, &v, NULL)) {
            return false;
        }
        parts[count++] = v;
        if (count < 3 && p < end && *p == ':') {
            p++;
            continue;
        }
        break;
    }
    if (count < 2) {
        return false;
    }
    u32 h = count == 3 ? parts[0] : 0;
    u32 m = count == 3 ? parts[1] : parts[0];
    u32 s = count == 3 ? parts[2] : parts[1];
    u32 frac = 0;
    if (p < end && (*p == ',' || *p == '.')) {
        p++;
        int digits = 0;
        if (srt_read_uint(&p, end, &frac, &digits)) {
            while (digits < 3) {
                frac *= 10;
                digits++;
            }
            while (digits > 3) {
                frac /= 10;
                digits--;
            }
        }
    }
    *ms_out = ((h * 60u + m) * 60u + s) * 1000u + frac;
    *pp = p;
    return true;
}

/* Appends one subtitle line to *w, dropping markup: <i>, <font ...>, ASS
 * override blocks like {\an8}, and turning \N into a line break. */
static void srt_append_clean_line(char *dst, size_t *w, const char *p, const char *end)
{
    size_t start = *w;
    while (p < end) {
        char c = *p;
        if (c == '<') {
            const char *q = p + 1;
            while (q < end && *q != '>' && q - p < 96) {
                q++;
            }
            if (q < end && *q == '>') {
                p = q + 1;
                continue;
            }
        } else if (c == '{') {
            const char *q = p + 1;
            while (q < end && *q != '}' && q - p < 256) {
                q++;
            }
            if (q < end && *q == '}') {
                p = q + 1;
                continue;
            }
        } else if (c == '\\' && p + 1 < end) {
            if (p[1] == 'N' || p[1] == 'n') {
                dst[(*w)++] = '\n';
                p += 2;
                continue;
            }
            if (p[1] == 'h') {
                dst[(*w)++] = ' ';
                p += 2;
                continue;
            }
        } else if (c == '&') {
            static const struct { const char *name; char ch; } entities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}
            };
            bool matched = false;
            for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
                size_t n = strlen(entities[i].name);
                if ((size_t)(end - p) >= n && strncmp(p, entities[i].name, n) == 0) {
                    dst[(*w)++] = entities[i].ch;
                    p += n;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }
        } else if (c == '\r' || c == '\t') {
            dst[(*w)++] = ' ';
            p++;
            continue;
        }
        dst[(*w)++] = c;
        p++;
    }

    /* Trim spaces at both ends of each produced line. */
    size_t r = start;
    size_t out = start;
    bool line_start = true;
    size_t last_non_space = start;
    while (r < *w) {
        char c = dst[r++];
        if (c == '\n') {
            /* last_non_space stays before the break, so a trailing break is dropped. */
            out = last_non_space;
            if (out > start && dst[out - 1] != '\n') {
                dst[out++] = '\n';
            }
            line_start = true;
            continue;
        }
        if (line_start && c == ' ') {
            continue;
        }
        line_start = false;
        dst[out++] = c;
        if (c != ' ') {
            last_non_space = out;
        }
    }
    *w = last_non_space;
}

static int subtitle_cue_compare(const void *a, const void *b)
{
    const SubtitleCue *x = (const SubtitleCue *)a;
    const SubtitleCue *y = (const SubtitleCue *)b;
    if (x->start_ms != y->start_ms) {
        return x->start_ms < y->start_ms ? -1 : 1;
    }
    return x->end_ms < y->end_ms ? -1 : (x->end_ms > y->end_ms ? 1 : 0);
}

static bool subtitle_parse_srt(const char *data, size_t size, SubtitleData *out)
{
    memset(out, 0, sizeof(*out));
    out->stream_index = -1;
    if (!data || !size) {
        return false;
    }

    const char *end = data + size;
    int max_cues = 0;
    for (const char *p = data; p + 3 <= end; p++) {
        if (p[0] == '-' && p[1] == '-' && p[2] == '>') {
            max_cues++;
            p += 2;
        }
    }
    if (max_cues <= 0) {
        return false;
    }

    out->cues = (SubtitleCue *)malloc((size_t)max_cues * sizeof(SubtitleCue));
    out->text = (char *)malloc(size + 2);
    if (!out->cues || !out->text) {
        subtitle_data_free(out);
        return false;
    }

    size_t w = 0;
    out->text[w++] = 0; /* offset 0 = empty string */
    const char *p = data;
    if (size >= 3 && (u8)p[0] == 0xEF && (u8)p[1] == 0xBB && (u8)p[2] == 0xBF) {
        p += 3;
    }

    while (p < end && out->count < max_cues) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) {
            line_end = end;
        }
        const char *arrow = NULL;
        for (const char *q = p; q + 3 <= line_end; q++) {
            if (q[0] == '-' && q[1] == '-' && q[2] == '>') {
                arrow = q;
                break;
            }
        }
        if (!arrow) {
            p = line_end < end ? line_end + 1 : end;
            continue;
        }

        u32 start_ms = 0;
        u32 end_ms = 0;
        const char *tp = p;
        const char *ep = arrow + 3;
        bool ok = srt_parse_time(&tp, arrow, &start_ms) && srt_parse_time(&ep, line_end, &end_ms);
        p = line_end < end ? line_end + 1 : end;
        if (!ok) {
            continue;
        }

        size_t text_start = w;
        bool any = false;
        while (p < end) {
            const char *le = memchr(p, '\n', (size_t)(end - p));
            if (!le) {
                le = end;
            }
            const char *content_end = le;
            if (content_end > p && content_end[-1] == '\r') {
                content_end--;
            }
            bool blank = true;
            for (const char *q = p; q < content_end; q++) {
                if (*q != ' ' && *q != '\t') {
                    blank = false;
                    break;
                }
            }
            if (blank) {
                p = le < end ? le + 1 : end;
                break;
            }
            if (any) {
                out->text[w++] = '\n';
            }
            size_t before = w;
            srt_append_clean_line(out->text, &w, p, content_end);
            if (w == before && any) {
                w--; /* drop the separator for a line that was only markup */
            } else if (w > before) {
                any = true;
            }
            p = le < end ? le + 1 : end;
        }
        if (!any || w == text_start) {
            w = text_start;
            continue;
        }
        out->text[w++] = 0;
        if (end_ms <= start_ms) {
            end_ms = start_ms + 2000u;
        }
        out->cues[out->count].start_ms = start_ms;
        out->cues[out->count].end_ms = end_ms;
        out->cues[out->count].text_off = (u32)text_start;
        out->count++;
    }

    if (out->count <= 0) {
        subtitle_data_free(out);
        return false;
    }

    bool sorted = true;
    for (int i = 1; i < out->count; i++) {
        if (out->cues[i].start_ms < out->cues[i - 1].start_ms) {
            sorted = false;
            break;
        }
    }
    if (!sorted) {
        qsort(out->cues, (size_t)out->count, sizeof(SubtitleCue), subtitle_cue_compare);
    }
    return true;
}

static void subtitle_loader_main(void *arg)
{
    (void)arg;
    SubtitleData parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.stream_index = -1;

    HttpResponse res;
    memset(&res, 0, sizeof(res));
    Result ret = api_get(g_sub_job_path, &res);
    if (R_SUCCEEDED(ret) && res.status >= 200 && res.status < 300 && res.body && res.size) {
        subtitle_parse_srt(res.body, res.size, &parsed);
    }
    free_response(&res);

    g_sub_job_result = parsed;
    __atomic_store_n(&g_sub_job_done, true, __ATOMIC_RELEASE);
}

static bool subtitles_start_job(int stream_index)
{
    if (g_sub_job_running || stream_index < 0 || !g_current.id[0]) {
        return false;
    }

    char item[160];
    char source[192];
    url_encode(g_current.id, item, sizeof(item));
    url_encode(g_play_media_source_id[0] ? g_play_media_source_id : g_current.id, source, sizeof(source));
    snprintf(g_sub_job_path, sizeof(g_sub_job_path), "/Videos/%s/%s/Subtitles/%d/0/Stream.srt", item, source, stream_index);

    memset(&g_sub_job_result, 0, sizeof(g_sub_job_result));
    g_sub_job_result.stream_index = -1;
    g_sub_job_index = stream_index;
    __atomic_store_n(&g_sub_job_done, false, __ATOMIC_RELEASE);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 2;
    if (prio > 0x3F) {
        prio = 0x3F;
    }
    g_sub_thread = threadCreate(subtitle_loader_main, NULL, 64 * 1024, prio, -2, false);
    g_sub_job_running = g_sub_thread != NULL;
    return g_sub_job_running;
}

static void subtitles_join_job(void)
{
    if (!g_sub_job_running) {
        return;
    }
    threadJoin(g_sub_thread, U64_MAX);
    threadFree(g_sub_thread);
    g_sub_thread = NULL;
    g_sub_job_running = false;
}

/* Main-thread pump: adopts finished downloads and starts the one the current
 * selection needs. Returns true when what the picker shows has changed. */
static bool subtitles_poll(void)
{
    bool changed = false;
    if (g_sub_job_running && __atomic_load_n(&g_sub_job_done, __ATOMIC_ACQUIRE)) {
        subtitles_join_job();
        SubtitleData result = g_sub_job_result;
        memset(&g_sub_job_result, 0, sizeof(g_sub_job_result));
        bool wanted = tracks_for_current_item() && g_sub_job_index == tracks_selected_text_sub_index();
        if (result.cues && wanted) {
            subtitle_data_free(&g_subs);
            g_subs = result;
            g_subs.stream_index = g_sub_job_index;
            copy_safe(g_subs.item_id, sizeof(g_subs.item_id), g_current.id);
            g_sub_generation++;
        } else {
            subtitle_data_free(&result);
            if (wanted) {
                g_sub_failed_index = g_sub_job_index;
                copy_safe(g_sub_failed_item, sizeof(g_sub_failed_item), g_current.id);
            }
        }
        changed = true;
    }

    if (!g_sub_job_running) {
        int want = tracks_selected_text_sub_index();
        bool have = g_subs.cues && g_subs.stream_index == want && strcmp(g_subs.item_id, g_current.id) == 0;
        if (want < 0) {
            if (g_subs.cues) {
                subtitle_data_free(&g_subs);
                g_sub_generation++;
                changed = true;
            }
        } else if (!have) {
            bool failed = g_sub_failed_index == want && strcmp(g_sub_failed_item, g_current.id) == 0;
            if (!failed) {
                if (g_subs.cues) {
                    subtitle_data_free(&g_subs);
                    g_sub_generation++;
                }
                if (!subtitles_start_job(want)) {
                    g_sub_failed_index = want; /* no thread: don't retry every frame */
                    copy_safe(g_sub_failed_item, sizeof(g_sub_failed_item), g_current.id);
                }
                changed = true;
            }
        }
    }
    return changed;
}

static SubtitleState subtitles_state(void)
{
    int want = tracks_selected_text_sub_index();
    if (want < 0) {
        return SUB_STATE_NONE;
    }
    if (g_subs.cues && g_subs.stream_index == want && strcmp(g_subs.item_id, g_current.id) == 0) {
        return SUB_STATE_READY;
    }
    if (g_sub_failed_index == want && strcmp(g_sub_failed_item, g_current.id) == 0) {
        return SUB_STATE_FAILED;
    }
    return SUB_STATE_LOADING;
}

/* Frees the loaded subtitles (waiting for an in-flight download first). */
static void subtitles_release(void)
{
    subtitles_join_job();
    subtitle_data_free(&g_sub_job_result);
    subtitle_data_free(&g_subs);
    g_sub_failed_index = -1;
    g_sub_failed_item[0] = 0;
    g_sub_generation++;
}

/* Cues showing at time ms: up to max_out indices, in start order. */
static int subtitles_active_cues(u32 ms, int *out, int max_out)
{
    if (!g_subs.cues || g_subs.count <= 0 || max_out <= 0) {
        return 0;
    }
    int lo = 0;
    int hi = g_subs.count - 1;
    int last = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_subs.cues[mid].start_ms <= ms) {
            last = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (last < 0) {
        return 0;
    }

    int found = 0;
    int tmp[8];
    for (int i = last; i >= 0 && last - i < 64 && found < 8; i--) {
        const SubtitleCue *c = &g_subs.cues[i];
        if (ms < c->end_ms && g_subs.text[c->text_off]) {
            tmp[found++] = i;
        }
    }
    int n = found < max_out ? found : max_out;
    for (int i = 0; i < n; i++) {
        out[i] = tmp[found - 1 - i];
    }
    return n;
}

/* ---- Picker state ---------------------------------------------------- */

typedef struct {
    bool open;
    int column; /* 0 audio, 1 subtitles */
    int cursor[2];
    int scroll[2];
} TrackMenu;

static TrackMenu g_track_menu;

typedef enum {
    TRACK_MENU_NONE,
    TRACK_MENU_REDRAW,
    TRACK_MENU_RESTART
} TrackMenuAction;

static int track_menu_rows(int column)
{
    return column == 0 ? g_audio_track_count : g_sub_track_count + 1;
}

static void track_menu_open(void)
{
    memset(&g_track_menu, 0, sizeof(g_track_menu));
    g_track_menu.open = true;

    int audio = tracks_effective_audio_index();
    for (int i = 0; i < g_audio_track_count; i++) {
        if (g_audio_tracks[i].index == audio) {
            g_track_menu.cursor[0] = i;
        }
    }
    for (int i = 0; i < g_sub_track_count; i++) {
        if (g_sub_tracks[i].index == g_sel_sub_index) {
            g_track_menu.cursor[1] = i + 1;
        }
    }
    g_track_menu.column = g_audio_track_count > 1 || g_sub_track_count == 0 ? 0 : 1;
}

static void track_menu_close(void)
{
    g_track_menu.open = false;
}

/* Handles a key press while the picker is open. Keys it uses are removed
 * from *down so the player doesn't also act on them. */
static TrackMenuAction track_menu_input(u32 *down)
{
    u32 keys = *down;
    *down &= ~(KEY_A | KEY_B | KEY_X | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (keys & (KEY_B | KEY_X)) {
        track_menu_close();
        return TRACK_MENU_REDRAW;
    }

    int col = g_track_menu.column;
    if (keys & (KEY_LEFT | KEY_RIGHT)) {
        g_track_menu.column = (keys & KEY_LEFT) ? 0 : 1;
        return g_track_menu.column != col ? TRACK_MENU_REDRAW : TRACK_MENU_NONE;
    }

    int rows = track_menu_rows(col);
    if (keys & (KEY_UP | KEY_DOWN)) {
        if (rows <= 0) {
            return TRACK_MENU_NONE;
        }
        int cur = g_track_menu.cursor[col] + ((keys & KEY_DOWN) ? 1 : -1);
        if (cur < 0) {
            cur = rows - 1;
        } else if (cur >= rows) {
            cur = 0;
        }
        g_track_menu.cursor[col] = cur;
        return TRACK_MENU_REDRAW;
    }

    if (!(keys & KEY_A) || rows <= 0) {
        return TRACK_MENU_NONE;
    }

    int cur = g_track_menu.cursor[col];
    if (col == 0) {
        const MediaTrack *track = &g_audio_tracks[cur];
        g_pref_audio_set = track->lang[0] != 0;
        copy_safe(g_pref_audio_lang, sizeof(g_pref_audio_lang), track->lang);
        if (track->index == tracks_effective_audio_index()) {
            return TRACK_MENU_NONE;
        }
        g_sel_audio_index = track->index;
        track_menu_close();
        return TRACK_MENU_RESTART;
    }

    const MediaTrack *old_sub = tracks_selected_sub();
    const MediaTrack *new_sub = cur > 0 ? &g_sub_tracks[cur - 1] : NULL;
    g_pref_sub_set = true;
    g_pref_sub_off = new_sub == NULL;
    copy_safe(g_pref_sub_lang, sizeof(g_pref_sub_lang), new_sub ? new_sub->lang : "");
    int new_index = new_sub ? new_sub->index : -1;
    if (new_index == g_sel_sub_index) {
        return TRACK_MENU_NONE;
    }
    bool restart = (old_sub && !old_sub->text) || (new_sub && !new_sub->text);
    g_sel_sub_index = new_index;
    if (new_sub && new_sub->text && g_sub_failed_index == new_index) {
        g_sub_failed_index = -1; /* picking it again retries the download */
    }
    if (restart) {
        track_menu_close();
        return TRACK_MENU_RESTART;
    }
    return TRACK_MENU_REDRAW;
}

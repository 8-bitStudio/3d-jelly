/* Song lyrics from Jellyfin 10.9+ (GET /Audio/{id}/Lyrics).
 *
 * Kept cheap for Old 3DS:
 *  - one small request per song on the UI thread through its own non-blocking
 *    curl multi handle, started only after the song's audio is playing (or
 *    right away if the lyrics screen is already open);
 *  - lines are word-wrapped into display rows once, measuring each word once;
 *  - each frame only the handful of rows on screen are drawn, and full-rate
 *    redraws are requested only while the list is moving. */
#define LYRICS_MAX_LINES 512
#define LYRICS_MAX_ROWS 1024
#define LYRICS_TEXT_CAP (24 * 1024)
/* Latin text reads fine small; kanji/kana/hangul strokes blur together at
 * that size, so songs with CJK lyrics use a larger font and taller rows. */
#define LYRICS_SCALE (g_lyrics.scale)
#define LYRICS_SCALE_LATIN 0.55f
#define LYRICS_SCALE_CJK 0.78f
#define LYRICS_WRAP 284.0f
#define LYRICS_ROW_H (g_lyrics.row_h)
#define LYRICS_LINE_GAP 9
#define LYRICS_TOP 30
#define LYRICS_BOTTOM 240
#define LYRICS_AREA_H (LYRICS_BOTTOM - LYRICS_TOP)
#define LYRICS_FOLLOW_DELAY_MS 3500
#define LYRICS_TAP_SLOP 7

static float text_width_for_scale(const char *text, float scale);
static int utf8_char_bytes(const char *s);
static bool ui_text_has_cjk_ideograph(const char *text);
static bool ui_text_has_kana(const char *text);
static bool ui_text_has_hangul(const char *text);

typedef enum { LYRICS_IDLE, LYRICS_LOADING, LYRICS_READY, LYRICS_NONE } LyricsState;
typedef struct { u64 start; u32 text; u16 first_row, rows; bool timed; } LyricLine;
typedef struct { s32 y; u32 text; u16 len, width, line; } LyricRow;

typedef struct {
    LyricsState state;
    bool open, synced, laid_out, snap, touching, dragged, animating, http_added;
    char item_id[80];
    CURLM *multi;
    CURL *http;
    struct curl_slist *headers;
    CurlResponseBuffer response;
    char text[LYRICS_TEXT_CAP];
    u32 text_used;
    LyricLine lines[LYRICS_MAX_LINES];
    int line_count;
    LyricRow rows[LYRICS_MAX_ROWS];
    int row_count, content_h, current, row_h;
    float scale;
    float scroll, velocity;
    int touch_y0, touch_last_y;
    u64 last_step_ms, manual_until;
} MusicLyrics;
static MusicLyrics g_lyrics;

static void lyrics_detach(void)
{
    if (g_lyrics.http_added) curl_multi_remove_handle(g_lyrics.multi, g_lyrics.http);
    g_lyrics.http_added = false;
    curl_slist_free_all(g_lyrics.headers);
    g_lyrics.headers = NULL;
    free(g_lyrics.response.data);
    memset(&g_lyrics.response, 0, sizeof(g_lyrics.response));
}

static void lyrics_reset_for(const char *item_id)
{
    lyrics_detach();
    g_lyrics.state = LYRICS_IDLE;
    copy_safe(g_lyrics.item_id, sizeof(g_lyrics.item_id), item_id);
    g_lyrics.synced = g_lyrics.laid_out = g_lyrics.touching = g_lyrics.dragged = g_lyrics.animating = false;
    g_lyrics.snap = true;
    g_lyrics.text_used = 0;
    g_lyrics.line_count = g_lyrics.row_count = g_lyrics.content_h = 0;
    g_lyrics.current = -1;
    g_lyrics.scroll = g_lyrics.velocity = 0;
    g_lyrics.manual_until = 0;
    g_lyrics.scale = LYRICS_SCALE_LATIN;
    g_lyrics.row_h = 19;
}

/* Called from music_stop: drops the connection and closes the screen. */
static void lyrics_shutdown(void)
{
    lyrics_reset_for("");
    if (g_lyrics.http) curl_easy_cleanup(g_lyrics.http);
    if (g_lyrics.multi) curl_multi_cleanup(g_lyrics.multi);
    g_lyrics.http = NULL;
    g_lyrics.multi = NULL;
    g_lyrics.open = false;
}

static bool lyrics_begin(void)
{
    if (!curl_http_runtime_init()) return false;
    char id[240], path[320], url[STREAM_URL_CAP], auth[384], header[448];
    url_encode(g_current.id, id, sizeof(id));
    snprintf(path, sizeof(path), "/Audio/%s/Lyrics", id);
    build_url(url, sizeof(url), path);
    auth_header(auth, sizeof(auth), true);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    if (!g_lyrics.multi) g_lyrics.multi = curl_multi_init();
    if (!g_lyrics.http) g_lyrics.http = curl_easy_init();
    if (!g_lyrics.multi || !g_lyrics.http) return false;
    /* Reset options but keep curl's connection cache for the next song. */
    curl_easy_reset(g_lyrics.http);
    g_lyrics.headers = curl_slist_append(NULL, header);
    g_lyrics.headers = curl_slist_append(g_lyrics.headers, "Accept: application/json");
    curl_easy_setopt(g_lyrics.http, CURLOPT_URL, url);
    curl_easy_setopt(g_lyrics.http, CURLOPT_HTTPHEADER, g_lyrics.headers);
    curl_easy_setopt(g_lyrics.http, CURLOPT_USERAGENT, "3dJelly/" APP_VERSION);
    curl_easy_setopt(g_lyrics.http, CURLOPT_CONNECTTIMEOUT_MS, 6000L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_TIMEOUT_MS, 12000L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(g_lyrics.http, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(g_lyrics.http, CURLOPT_WRITEDATA, &g_lyrics.response);
    g_lyrics.http_added = curl_multi_add_handle(g_lyrics.multi, g_lyrics.http) == CURLM_OK;
    return g_lyrics.http_added;
}

static bool lyrics_parse(const char *body, const char *end)
{
    const char *p = find_key_range(body, end, "Lyrics");
    if (!p || p >= end || *p != '[') return false;
    ++p;
    int timed = 0, nonzero = 0, words = 0;
    char line[512];
    g_lyrics.line_count = 0;
    g_lyrics.text_used = 0;
    while (g_lyrics.line_count < LYRICS_MAX_LINES) {
        const char *object, *object_end;
        if (!json_object_range_after(p, end, &object, &object_end)) break;
        json_get_string_range(object, object_end, "Text", line, sizeof(line));
        /* One display line per lyric line: flatten stray control characters. */
        for (char *c = line; *c; ++c) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
        size_t len = strlen(line);
        while (len && line[len - 1] == ' ') line[--len] = 0;
        if (g_lyrics.text_used + len + 1 > LYRICS_TEXT_CAP) break;
        const char *start = find_key_range(object, object_end, "Start");
        LyricLine *out = &g_lyrics.lines[g_lyrics.line_count++];
        out->timed = start && start < object_end && *start >= '0' && *start <= '9';
        out->start = out->timed ? strtoull(start, NULL, 10) : 0;
        out->text = g_lyrics.text_used;
        memcpy(g_lyrics.text + g_lyrics.text_used, line, len + 1);
        g_lyrics.text_used += len + 1;
        if (out->timed) { ++timed; if (out->start) ++nonzero; }
        if (len) ++words;
        p = skip_ws(object_end, end);
        if (p >= end || *p != ',') break;
        ++p;
    }
    if (!words) { g_lyrics.line_count = 0; return false; }
    bool cjk = false;
    for (int i = 0; i < g_lyrics.line_count && !cjk; ++i) {
        const char *text = g_lyrics.text + g_lyrics.lines[i].text;
        cjk = ui_text_has_cjk_ideograph(text) || ui_text_has_kana(text) || ui_text_has_hangul(text);
    }
    g_lyrics.scale = cjk ? LYRICS_SCALE_CJK : LYRICS_SCALE_LATIN;
    g_lyrics.row_h = cjk ? 26 : 19;
    g_lyrics.synced = nonzero > 0 && timed * 2 >= g_lyrics.line_count;
    if (g_lyrics.synced) {
        /* Untimed lines inherit the previous time so seeking stays in order. */
        u64 last = 0;
        for (int i = 0; i < g_lyrics.line_count; ++i) {
            LyricLine *l = &g_lyrics.lines[i];
            if (!l->timed || l->start < last) { l->start = last; }
            last = l->start;
        }
    }
    return true;
}

static void lyrics_tick(void)
{
    if (!g_music.active) return;
    if (strcmp(g_lyrics.item_id, g_current.id)) lyrics_reset_for(g_current.id);
    if (g_lyrics.state == LYRICS_IDLE) {
        /* Let the song's own stream connect first unless lyrics are on screen. */
        if (!g_lyrics.open && !g_music.started) return;
        g_lyrics.state = lyrics_begin() ? LYRICS_LOADING : LYRICS_NONE;
        if (g_lyrics.state == LYRICS_NONE) lyrics_detach();
        return;
    }
    if (g_lyrics.state != LYRICS_LOADING) return;
    int running = 0, remaining = 0;
    if (curl_multi_perform(g_lyrics.multi, &running) != CURLM_OK) {
        lyrics_detach(); g_lyrics.state = LYRICS_NONE; return;
    }
    CURLMsg *message = curl_multi_info_read(g_lyrics.multi, &remaining);
    if (!message || message->msg != CURLMSG_DONE) return;
    long status = 0;
    curl_easy_getinfo(g_lyrics.http, CURLINFO_RESPONSE_CODE, &status);
    bool ok = message->data.result == CURLE_OK && status == 200 && g_lyrics.response.data &&
        lyrics_parse(g_lyrics.response.data, g_lyrics.response.data + g_lyrics.response.size);
    lyrics_detach();
    g_lyrics.state = ok ? LYRICS_READY : LYRICS_NONE;
}

static bool lyrics_emit_row(int line, const char *start, size_t len, float width, int *y)
{
    if (g_lyrics.row_count >= LYRICS_MAX_ROWS) return false;
    LyricRow *row = &g_lyrics.rows[g_lyrics.row_count++];
    row->y = *y;
    row->text = (u32)(start - g_lyrics.text);
    row->len = (u16)len;
    row->width = (u16)(width + 0.5f);
    row->line = (u16)line;
    g_lyrics.lines[line].rows++;
    *y += LYRICS_ROW_H;
    return true;
}

static float lyrics_measure(const char *start, size_t len)
{
    char buf[256];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = 0;
    return text_width_for_scale(buf, LYRICS_SCALE);
}

/* Word-wrap every line into rows once. Word widths add up, so each word is
 * measured once; only a word wider than the screen is split per character
 * (common for CJK lines, which have no spaces). */
static void lyrics_layout(void)
{
    float space = text_width_for_scale("a a", LYRICS_SCALE) - text_width_for_scale("aa", LYRICS_SCALE);
    int y = 0;
    g_lyrics.row_count = 0;
    for (int i = 0; i < g_lyrics.line_count; ++i) {
        LyricLine *line = &g_lyrics.lines[i];
        line->first_row = (u16)g_lyrics.row_count;
        line->rows = 0;
        const char *p = g_lyrics.text + line->text;
        if (!*p) {
            /* Blank line: an instrumental gap when synced, a stanza break otherwise. */
            if (!lyrics_emit_row(i, p, 0, 0, &y)) break;
            if (!g_lyrics.synced) y -= LYRICS_ROW_H / 2;
            y += LYRICS_LINE_GAP;
            continue;
        }
        const char *row_start = NULL;
        size_t row_len = 0;
        float row_w = 0;
        bool full = false;
        while (*p && !full) {
            while (*p == ' ') ++p;
            if (!*p) break;
            const char *word = p;
            while (*p && *p != ' ') p += utf8_char_bytes(p);
            float w = lyrics_measure(word, (size_t)(p - word));
            if (row_start && row_w + space + w <= LYRICS_WRAP) {
                row_len = (size_t)(p - row_start);
                row_w += space + w;
                continue;
            }
            if (row_start && !lyrics_emit_row(i, row_start, row_len, row_w, &y)) { full = true; break; }
            row_start = NULL;
            if (w <= LYRICS_WRAP) {
                row_start = word; row_len = (size_t)(p - word); row_w = w;
                continue;
            }
            const char *c = word;
            while (c < p) {
                const char *e = c;
                float fit = 0;
                while (e < p) {
                    const char *next = e + utf8_char_bytes(e);
                    float pw = lyrics_measure(c, (size_t)(next - c));
                    if (pw > LYRICS_WRAP && e > c) break;
                    fit = pw; e = next;
                }
                if (e < p) {
                    if (!lyrics_emit_row(i, c, (size_t)(e - c), fit, &y)) { full = true; break; }
                } else {
                    row_start = c; row_len = (size_t)(p - c); row_w = fit;
                }
                c = e;
            }
        }
        if (!full && row_start && !lyrics_emit_row(i, row_start, row_len, row_w, &y)) full = true;
        y += LYRICS_LINE_GAP;
        if (full) { g_lyrics.line_count = i + 1; break; }
    }
    g_lyrics.content_h = y;
    g_lyrics.laid_out = true;
    g_lyrics.snap = true;
}

/* Synced lyrics pad the top and bottom by half a screen so any line, even
 * the first or last, can sit in the middle while it is sung. */
static float lyrics_pad_top(void)
{
    return g_lyrics.synced ? LYRICS_AREA_H / 2 - LYRICS_ROW_H / 2 : 8;
}

static float lyrics_max_scroll(void)
{
    float bottom = g_lyrics.synced ? LYRICS_AREA_H / 2 : 8;
    float max = lyrics_pad_top() + g_lyrics.content_h + bottom - LYRICS_AREA_H;
    return max > 0 ? max : 0;
}

static float lyrics_line_target(int index)
{
    if (index < 0 || index >= g_lyrics.line_count) return 0;
    const LyricLine *line = &g_lyrics.lines[index];
    if (!line->rows) return 0;
    float y = g_lyrics.rows[line->first_row].y;
    return y + (line->rows - 1) * LYRICS_ROW_H / 2.0f;
}

static int lyrics_line_at_time(u64 position)
{
    int lo = 0, hi = g_lyrics.line_count - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_lyrics.lines[mid].start <= position) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return found;
}

/* First row whose bottom is below content coordinate y. */
static int lyrics_row_from_y(float y)
{
    int lo = 0, hi = g_lyrics.row_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_lyrics.rows[mid].y + LYRICS_ROW_H + LYRICS_LINE_GAP <= y) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void lyrics_tap(int screen_y)
{
    if (!g_lyrics.synced || !g_lyrics.laid_out || !g_lyrics.row_count) return;
    float y = screen_y - LYRICS_TOP - lyrics_pad_top() + g_lyrics.scroll;
    if (y < 0) return;
    int row = lyrics_row_from_y(y);
    if (row >= g_lyrics.row_count) return;
    int line = g_lyrics.rows[row].line;
    g_lyrics.current = line;
    g_lyrics.manual_until = 0;
    g_lyrics.velocity = 0;
    music_seek(g_lyrics.lines[line].start);
}

static void lyrics_open(void)
{
    if (strcmp(g_lyrics.item_id, g_current.id)) lyrics_reset_for(g_current.id);
    g_lyrics.open = true;
    g_lyrics.snap = true;
    g_lyrics.manual_until = 0;
    g_lyrics.velocity = 0;
    g_lyrics.touching = false;
    g_lyrics.last_step_ms = osGetTime();
}

static void lyrics_manual(u64 now)
{
    g_lyrics.manual_until = now + LYRICS_FOLLOW_DELAY_MS;
}

/* Runs every frame while the lyrics screen is open: touch drag/fling/tap,
 * D-pad scrolling, and easing toward the line being sung. */
static void lyrics_update(u32 down, u32 held)
{
    u64 now = osGetTime();
    float dt = (now - g_lyrics.last_step_ms) / 1000.0f;
    if (dt < 0 || dt > 0.1f) dt = 0.1f;
    g_lyrics.last_step_ms = now;
    if (g_lyrics.state == LYRICS_READY && !g_lyrics.laid_out) lyrics_layout();
    bool ready = g_lyrics.state == LYRICS_READY && g_lyrics.laid_out;

    if (held & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        if (!g_lyrics.touching) {
            if (down & KEY_TOUCH) {
                g_lyrics.touching = true;
                g_lyrics.dragged = false;
                g_lyrics.touch_y0 = g_lyrics.touch_last_y = t.py;
                g_lyrics.velocity = 0;
            }
        } else {
            int dy = t.py - g_lyrics.touch_last_y;
            if (!g_lyrics.dragged && abs(t.py - g_lyrics.touch_y0) > LYRICS_TAP_SLOP) {
                g_lyrics.dragged = true;
                dy = t.py - g_lyrics.touch_y0;
            }
            if (g_lyrics.dragged && ready) {
                g_lyrics.scroll -= dy;
                if (dt > 0) g_lyrics.velocity = g_lyrics.velocity * 0.5f + (-dy / dt) * 0.5f;
                lyrics_manual(now);
            }
            g_lyrics.touch_last_y = t.py;
        }
    } else if (g_lyrics.touching) {
        g_lyrics.touching = false;
        if (!g_lyrics.dragged) {
            if (g_lyrics.touch_y0 < LYRICS_TOP - 4) { g_lyrics.open = false; return; }
            if (ready) lyrics_tap(g_lyrics.touch_y0);
        } else lyrics_manual(now);
    }

    if (!ready) { g_lyrics.animating = false; return; }

    if (held & (KEY_UP | KEY_DOWN)) {
        g_lyrics.scroll += ((held & KEY_DOWN) ? 1 : -1) * 220.0f * dt;
        g_lyrics.velocity = 0;
        lyrics_manual(now);
    }

    if (!g_lyrics.touching && g_lyrics.velocity != 0) {
        g_lyrics.scroll += g_lyrics.velocity * dt;
        g_lyrics.velocity *= 1.0f - (dt * 5.0f > 1.0f ? 1.0f : dt * 5.0f);
        if (g_lyrics.velocity > -15 && g_lyrics.velocity < 15) g_lyrics.velocity = 0;
        else lyrics_manual(now);
    }

    if (g_lyrics.synced) g_lyrics.current = lyrics_line_at_time(music_position());

    bool follow = g_lyrics.synced && !g_lyrics.touching && g_lyrics.velocity == 0 && now >= g_lyrics.manual_until;
    float max = lyrics_max_scroll();
    if (g_lyrics.snap) {
        g_lyrics.scroll = g_lyrics.synced ? lyrics_line_target(g_lyrics.current) : 0;
        g_lyrics.snap = false;
    } else if (follow) {
        float target = lyrics_line_target(g_lyrics.current);
        if (target > max) target = max;
        float diff = target - g_lyrics.scroll;
        float step = dt * 9.0f;
        g_lyrics.scroll += diff * (step > 1 ? 1 : step);
        if (diff > -0.5f && diff < 0.5f) g_lyrics.scroll = target;
    }
    if (g_lyrics.scroll < 0) { g_lyrics.scroll = 0; g_lyrics.velocity = 0; }
    if (g_lyrics.scroll > max) { g_lyrics.scroll = max; g_lyrics.velocity = 0; }

    float target = follow ? lyrics_line_target(g_lyrics.current) : g_lyrics.scroll;
    if (target > max) target = max;
    float gap = target - g_lyrics.scroll;
    g_lyrics.animating = g_lyrics.touching || g_lyrics.velocity != 0 ||
        (held & (KEY_UP | KEY_DOWN)) || gap > 0.5f || gap < -0.5f;
}

/* The main loop draws music at a low rate; this asks for full-rate frames
 * only while the lyrics are scrolling or being dragged. */
static bool lyrics_needs_frame(void)
{
    return g_lyrics.open && g_view == VIEW_MUSIC && (g_lyrics.animating || g_lyrics.touching ||
        (g_lyrics.state == LYRICS_READY && !g_lyrics.laid_out));
}

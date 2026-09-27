/* Top-screen shelves/grid; the touch screen always describes the focused item. */
#define BROWSE_CACHE_COUNT 24
#define BROWSE_CONTENT_TOP 40
#define BROWSE_CONTENT_BOTTOM 220
#define GRID_COLUMNS 4
#define HOME_LANDSCAPE_COLUMNS 2
#define HOME_CARD_TOP 22
#define GRID_CARD_TOP 66
#define GRID_ROW_HEIGHT 146
#define EPISODE_ROW_HEIGHT 74
#define EPISODE_VISIBLE_ROWS 2
#define BROWSE_SCROLL_MS 180
#define BROWSE_ACTION_TOP 182
#define BROWSE_ACTION_BOTTOM 209
#define BROWSE_MENU_TOP 213
#define BROWSE_MENU_BOTTOM 239
typedef enum {
    BROWSE_ART_LANDSCAPE,
    BROWSE_ART_POSTER,
    BROWSE_ART_EPISODE,
    BROWSE_ART_SQUARE
} BrowseArtKind;
typedef struct {
    float value, from, target;
    u64 started;
    bool ready;
} BrowseScroll;
typedef struct {
    char id[80];
    bool poster, thumb, square, ready, failed;
    unsigned used;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
} BrowseImage;
static BrowseImage g_browse_images[BROWSE_CACHE_COUNT];
static unsigned g_browse_image_clock;
static CURLM *g_browse_multi;
static CURL *g_browse_http;
static struct curl_slist *g_browse_headers;
static CurlResponseBuffer g_browse_response;
static int g_browse_request_row = -1;
static int g_browse_request_image = -1;
static char g_browse_server[256];
static char g_browse_user[80];
static int g_browse_repeat_key;
static u64 g_browse_repeat_at;
static u64 g_browse_notice_until;
static BrowseScroll g_browse_scroll;
static View g_browse_scroll_view;
static char g_browse_scroll_parent[80], g_browse_scroll_search[96];
static bool g_music_mode_focus;
static int g_music_nav_mode;
static bool music_browse_view(void);
static bool music_track_list(void);
static void draw_music_browse(void);
static void draw_music_preview(const MediaItem *item);
static void music_activate_mode(void);
static const char *music_mode_name(int mode);

static void browse_scroll_step(BrowseScroll *scroll, float target, u64 now)
{
    if (!scroll->ready) {
        *scroll = (BrowseScroll){ .value = target, .from = target, .target = target,
                                  .started = now, .ready = true };
        return;
    }
    /* Evaluate the old movement first so a reversal starts exactly where we are. */
    float t = now >= scroll->started ? (float)(now - scroll->started) / BROWSE_SCROLL_MS : 1;
    if (t >= 1) scroll->value = scroll->target;
    else {
        float remaining = 1 - t;
        scroll->value = scroll->target + (scroll->from - scroll->target) * remaining * remaining * remaining;
    }
    if (target != scroll->target) {
        scroll->from = scroll->value;
        scroll->target = target;
        scroll->started = now;
    }
}

static bool browse_is_view(void)
{
    return g_view == VIEW_LIBRARIES || g_view == VIEW_ITEMS || g_view == VIEW_SEARCH;
}

static bool browse_is_episode_list(void)
{
    return g_view == VIEW_ITEMS && !strcmp(g_current_parent_type, "Season");
}

static void browse_cancel_request(void)
{
    if (g_browse_http) {
        curl_multi_remove_handle(g_browse_multi, g_browse_http);
        curl_easy_cleanup(g_browse_http);
    }
    if (g_browse_multi) curl_multi_cleanup(g_browse_multi);
    curl_slist_free_all(g_browse_headers);
    free(g_browse_response.data);
    memset(&g_browse_response, 0, sizeof(g_browse_response));
    g_browse_http = NULL;
    g_browse_multi = NULL;
    g_browse_headers = NULL;
    g_browse_request_row = g_browse_request_image = -1;
}

static void browse_shutdown(void)
{
    browse_cancel_request();
    for (int i = 0; i < BROWSE_CACHE_COUNT; ++i) {
        if (g_browse_images[i].ready) C3D_TexDelete(&g_browse_images[i].tex);
    }
    memset(g_browse_images, 0, sizeof(g_browse_images));
}

static bool browse_begin_request(const char *path)
{
    if (!curl_http_runtime_init()) return false;
    g_browse_multi = curl_multi_init();
    g_browse_http = curl_easy_init();
    if (!g_browse_multi || !g_browse_http) { browse_cancel_request(); return false; }
    char url[1024], auth[384], header[448];
    build_url(url, sizeof(url), path);
    auth_header(auth, sizeof(auth), true);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    g_browse_headers = curl_slist_append(NULL, header);
    curl_easy_setopt(g_browse_http, CURLOPT_URL, url);
    curl_easy_setopt(g_browse_http, CURLOPT_HTTPHEADER, g_browse_headers);
    curl_easy_setopt(g_browse_http, CURLOPT_USERAGENT, "3dJelly/" APP_VERSION);
    curl_easy_setopt(g_browse_http, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(g_browse_http, CURLOPT_WRITEDATA, &g_browse_response);
    curl_easy_setopt(g_browse_http, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(g_browse_http, CURLOPT_TIMEOUT_MS, 12000L);
    curl_easy_setopt(g_browse_http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_browse_http, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_browse_http, CURLOPT_MAXREDIRS, 3L);
    /* Match the app's existing support for self-signed Jellyfin servers. */
    curl_easy_setopt(g_browse_http, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(g_browse_http, CURLOPT_SSL_VERIFYHOST, 0L);
    if (curl_multi_add_handle(g_browse_multi, g_browse_http) != CURLM_OK) {
        browse_cancel_request();
        return false;
    }
    return true;
}

static MediaItem *home_items(int row, int *count, int **selected, int **first)
{
    if (!row) {
        *count = g_library_count;
        *selected = &g_library_selected;
        *first = &g_library_first;
        return g_libraries;
    }
    HomeRow *r = &g_home_rows[row - 1];
    *count = r->count;
    *selected = &r->selected;
    *first = &r->first;
    return r->items;
}

static int home_height(int row)
{
    if (row && g_home_rows[row - 1].loaded && !g_home_rows[row - 1].count) return 56;
    /* Leave the next heading and a strip of its artwork visible below My Media. */
    return row < 3 ? 150 : 168;
}

static int home_y(int row)
{
    int y = 0;
    for (int i = 0; i < row; ++i) y += home_height(i);
    return y;
}

static void browse_clamp(void)
{
    if (g_home_row >= g_library_count + 3) g_home_row = g_library_count + 2;
    if (g_home_row < 0) g_home_row = 0;
    if (g_nav_tab < 0) g_nav_tab = 0;
    if (g_nav_tab >= g_library_count + 2) g_nav_tab = g_library_count + 1;
    if (g_view == VIEW_LIBRARIES) {
        int count, *selected, *first;
        home_items(g_home_row, &count, &selected, &first);
        if (*selected >= count) *selected = count > 0 ? count - 1 : 0;
        if (*selected < 0) *selected = 0;
        int columns = g_home_row < 3 ? HOME_LANDSCAPE_COLUMNS : GRID_COLUMNS;
        if (*first > *selected) *first = *selected;
        if (*selected >= *first + columns) *first = *selected - columns + 1;
        int visible = BROWSE_CONTENT_BOTTOM - BROWSE_CONTENT_TOP;
        int max_scroll = home_y(g_library_count + 3) - visible;
        g_home_scroll = home_y(g_home_row);
        if (g_home_scroll > max_scroll) g_home_scroll = max_scroll;
        if (g_home_scroll < 0) g_home_scroll = 0;
    } else {
        if (g_selected >= g_item_count) g_selected = g_item_count > 0 ? g_item_count - 1 : 0;
        if (g_selected < 0) g_selected = 0;
        if (music_track_list()) {
            if (g_selected < g_scroll) g_scroll = g_selected;
            if (g_selected >= g_scroll + 3) g_scroll = g_selected - 2;
            int last = g_item_count > 3 ? g_item_count - 3 : 0;
            if (g_scroll > last) g_scroll = last;
            if (g_scroll < 0) g_scroll = 0;
        } else if (music_browse_view()) {
            g_scroll = g_selected / 3 * 3;
        } else if (browse_is_episode_list()) {
            /* Keep both visible episodes in place until focus reaches an edge. */
            if (g_selected < g_scroll) g_scroll = g_selected;
            if (g_selected >= g_scroll + EPISODE_VISIBLE_ROWS) g_scroll = g_selected - EPISODE_VISIBLE_ROWS + 1;
            int last = g_item_count > EPISODE_VISIBLE_ROWS ? g_item_count - EPISODE_VISIBLE_ROWS : 0;
            if (g_scroll > last) g_scroll = last;
            if (g_scroll < 0) g_scroll = 0;
        } else {
            /* One full poster row, with the following row peeking below it. */
            g_scroll = (g_selected / GRID_COLUMNS) * GRID_COLUMNS;
        }
    }
}

static void browse_update_scroll(void)
{
    if (g_browse_scroll_view != g_view || strcmp(g_browse_scroll_parent, g_current_parent_id) ||
        strcmp(g_browse_scroll_search, g_search_query)) {
        g_browse_scroll.ready = false;
        g_browse_scroll_view = g_view;
        copy_safe(g_browse_scroll_parent, sizeof(g_browse_scroll_parent), g_current_parent_id);
        copy_safe(g_browse_scroll_search, sizeof(g_browse_scroll_search), g_search_query);
    }
    float target = g_view == VIEW_LIBRARIES ? g_home_scroll : music_track_list() ? g_scroll * 48 :
        music_browse_view() ? g_scroll / 3 * 150 : browse_is_episode_list() ?
        g_scroll * EPISODE_ROW_HEIGHT : (g_scroll / GRID_COLUMNS) * GRID_ROW_HEIGHT;
    browse_scroll_step(&g_browse_scroll, target, osGetTime());
}

static MediaItem *browse_focused(void)
{
    if (g_music_mode_focus && music_browse_view()) return NULL;
    if (g_tab_focus) {
        return g_nav_tab >= 2 && g_nav_tab - 2 < g_library_count ? &g_libraries[g_nav_tab - 2] : NULL;
    }
    if (g_view == VIEW_LIBRARIES) {
        int count, *selected, *first;
        MediaItem *items = home_items(g_home_row, &count, &selected, &first);
        return count && *selected < count ? &items[*selected] : NULL;
    }
    return g_selected < g_item_count ? &g_items[g_selected] : NULL;
}

static const char *browse_image_id(const MediaItem *item, BrowseArtKind kind)
{
    if (kind == BROWSE_ART_SQUARE && music_is_audio(item) && !item->primary_image && item->album_id[0]) return item->album_id;
    if (item->series_id[0] && !strcmp(item->type, "Episode") &&
        (kind == BROWSE_ART_POSTER || (kind == BROWSE_ART_LANDSCAPE && item->series_thumb)))
        return item->series_id;
    return item->id;
}

static bool browse_image_is_thumb(const MediaItem *item, BrowseArtKind kind)
{
    return kind == BROWSE_ART_LANDSCAPE && item->series_thumb && strcmp(browse_image_id(item, kind), item->id);
}

static void browse_build_image_path(const MediaItem *item, BrowseArtKind kind, char *path, size_t size)
{
    char id[240];
    url_encode(browse_image_id(item, kind), id, sizeof(id));
    bool poster = kind == BROWSE_ART_POSTER;
    bool square = kind == BROWSE_ART_SQUARE;
    snprintf(path, size, "/Items/%s/Images/%s?fillWidth=%d&fillHeight=%d&quality=85&format=jpg",
             id, browse_image_is_thumb(item, kind) ? "Thumb" : "Primary", square ? 192 : poster ? 112 : 184, square ? 192 : poster ? 160 : 104);
}

static BrowseImage *browse_find_image(const MediaItem *item, BrowseArtKind kind, bool create)
{
    const char *id = browse_image_id(item, kind);
    bool poster = kind == BROWSE_ART_POSTER, thumb = browse_image_is_thumb(item, kind), square = kind == BROWSE_ART_SQUARE;
    int oldest = -1;
    for (int i = 0; i < BROWSE_CACHE_COUNT; ++i) {
        BrowseImage *entry = &g_browse_images[i];
        if (entry->id[0] && !strcmp(entry->id, id) && entry->poster == poster && entry->thumb == thumb && entry->square == square) {
            entry->used = ++g_browse_image_clock;
            return entry;
        }
        if (i != g_browse_request_image && (oldest < 0 || entry->used < g_browse_images[oldest].used)) oldest = i;
    }
    if (!create || oldest < 0) return NULL;
    BrowseImage *entry = &g_browse_images[oldest];
    if (entry->ready) C3D_TexDelete(&entry->tex);
    memset(entry, 0, sizeof(*entry));
    copy_safe(entry->id, sizeof(entry->id), id);
    entry->poster = poster;
    entry->thumb = thumb;
    entry->square = square;
    entry->used = ++g_browse_image_clock;
    return entry;
}

static bool browse_upload_image(BrowseImage *entry)
{
    int w = entry->square ? 192 : entry->poster ? 112 : 184, h = entry->square ? 192 : entry->poster ? 160 : 104;
    u16 *decoded = malloc(JPEG_PIXELS_CAP * sizeof(u16));
    u16 *pixels = malloc(256 * 256 * sizeof(u16));
    u16 *tiled = malloc(256 * 256 * sizeof(u16));
    int iw = 0, ih = 0;
    unsigned status = 0;
    bool ok = decoded && pixels && tiled &&
        decode_jpeg_rgb565((const u8 *)g_browse_response.data, g_browse_response.size, decoded, &iw, &ih, &status) &&
        browse_scale_into_art_texture(pixels, decoded, iw, ih, w, h, true) &&
        C3D_TexInit(&entry->tex, 256, 256, GPU_RGB565);
    if (ok) {
        browse_extrude_art_edges(pixels, w, h);
        browse_swizzle_rgb565(pixels, tiled, 256, 256);
        C3D_TexSetFilter(&entry->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&entry->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexUpload(&entry->tex, tiled);
        C3D_TexFlush(&entry->tex);
        entry->sub = (Tex3DS_SubTexture){ .width = w, .height = h, .left = 0,
            .right = w / 256.0f, .top = 1, .bottom = 1 - h / 256.0f };
        entry->ready = true;
    }
    free(decoded); free(pixels); free(tiled);
    return ok;
}

static bool browse_request_art(const MediaItem *item, BrowseArtKind kind)
{
    if (!item) return false;
    BrowseImage *entry = browse_find_image(item, kind, true);
    if (!entry || entry->ready || entry->failed) return false;
    char path[400];
    browse_build_image_path(item, kind, path, sizeof(path));
    if (!browse_begin_request(path)) { entry->failed = true; return false; }
    g_browse_request_image = (int)(entry - g_browse_images);
    return true;
}

static bool browse_request_row(int row)
{
    if (row < 1 || row >= g_library_count + 3 || g_home_rows[row - 1].loaded) return false;
    char path[768];
    build_home_row_path(row, path, sizeof(path));
    if (!browse_begin_request(path)) {
        g_home_rows[row - 1].loaded = g_home_rows[row - 1].failed = true;
        return false;
    }
    g_browse_request_row = row;
    return true;
}

static void browse_tick(void)
{
    bool playing_music = g_view == VIEW_MUSIC || (g_view == VIEW_MUSIC_SETTINGS && g_music.active);
    if (!browse_is_view() && !playing_music) { g_browse_scroll.ready = false; return; }
    if (!music_browse_view()) g_music_mode_focus = false;
    if (strcmp(g_browse_server, g_cfg.server) || strcmp(g_browse_user, g_cfg.user_id)) {
        browse_shutdown();
        copy_safe(g_browse_server, sizeof(g_browse_server), g_cfg.server);
        copy_safe(g_browse_user, sizeof(g_browse_user), g_cfg.user_id);
        g_browse_scroll.ready = false;
    }
    if (g_home_dirty) {
        browse_cancel_request();
        /* Only playback rows need refreshing; retain library/recent navigation. */
        memset(g_home_rows, 0, 2 * sizeof(HomeRow));
        g_home_dirty = false;
    }
    if (!playing_music) browse_clamp();
    /* Advance even while a network request is pending. */
    if (!playing_music) browse_update_scroll();
    bool moving = !playing_music && g_browse_scroll.value != g_browse_scroll.target;
    if (g_browse_http) {
        int running = 0, remaining = 0;
        CURLMcode result = curl_multi_perform(g_browse_multi, &running);
        /* JPEG decoding/uploading is synchronous; leave it until movement settles. */
        if (g_browse_request_image >= 0 && moving && result == CURLM_OK) return;
        CURLMsg *msg = curl_multi_info_read(g_browse_multi, &remaining);
        if (!msg && running && result == CURLM_OK) return;
        long status = 0;
        curl_easy_getinfo(g_browse_http, CURLINFO_RESPONSE_CODE, &status);
        bool ok = msg && msg->msg == CURLMSG_DONE && msg->data.result == CURLE_OK &&
                  status == 200 && g_browse_response.data;
        if (g_browse_request_row > 0) {
            HomeRow *row = &g_home_rows[g_browse_request_row - 1];
            row->count = 0;
            row->loaded = true;
            row->failed = !ok || !parse_items(g_browse_response.data, row->items, &row->count,
                                               HOME_ROW_ITEMS, true, NULL);
        } else if (g_browse_request_image >= 0) {
            BrowseImage *entry = &g_browse_images[g_browse_request_image];
            entry->failed = !ok || !browse_upload_image(entry);
        }
        browse_cancel_request();
        return;
    }
    if (g_view == VIEW_LIBRARIES && browse_request_row(g_home_row)) return;
    if (moving) return;
    MediaItem *focused = playing_music ? &g_current : browse_focused();
    BrowseArtKind focused_art = music_is_item(focused) ? BROWSE_ART_SQUARE : browse_is_episode_list() && !g_tab_focus ? BROWSE_ART_EPISODE :
        browse_item_prefers_poster_art(focused) ? BROWSE_ART_POSTER : BROWSE_ART_LANDSCAPE;
    if (focused && browse_request_art(focused, focused_art)) return;
    if (playing_music) return;
    if (g_view == VIEW_LIBRARIES) {
        /* Fetch the two watch rows early, then only visible recently-added rows. */
        if (browse_request_row(1) || browse_request_row(2)) return;
        for (int r = 0; r < g_library_count + 3; ++r) {
            float y = BROWSE_CONTENT_TOP + home_y(r) - g_browse_scroll.value;
            if (y + home_height(r) < BROWSE_CONTENT_TOP || y >= BROWSE_CONTENT_BOTTOM) continue;
            if (browse_request_row(r)) return;
            int count, *selected, *first;
            MediaItem *items = home_items(r, &count, &selected, &first);
            for (int i = *first; i < count && i < *first + (r < 3 ? HOME_LANDSCAPE_COLUMNS : GRID_COLUMNS); ++i)
                if (browse_request_art(&items[i], music_is_item(&items[i]) ? BROWSE_ART_SQUARE : r >= 3 ? BROWSE_ART_POSTER : BROWSE_ART_LANDSCAPE)) return;
        }
    } else if (music_browse_view()) {
        for (int i = g_scroll; i < g_item_count && i < g_scroll + 6; ++i)
            if (browse_request_art(&g_items[i], BROWSE_ART_SQUARE)) return;
    } else if (browse_is_episode_list()) {
        for (int i = g_scroll > 0 ? g_scroll - 1 : 0; i < g_item_count && i < g_scroll + 4; ++i)
            if (browse_request_art(&g_items[i], BROWSE_ART_EPISODE)) return;
    } else {
        for (int i = g_scroll; i < g_item_count && i < g_scroll + GRID_COLUMNS * 2; ++i)
            if (browse_request_art(&g_items[i], music_is_item(&g_items[i]) ? BROWSE_ART_SQUARE : BROWSE_ART_POSTER)) return;
    }
}

static void browse_label(float x, float y, float width, float scale, u32 color, const char *label)
{
    char text[256];
    copy_safe(text, sizeof(text), label);
    trim_text_to_width(text, sizeof(text), scale, width, true);
    draw_text(x, y, scale, color, "%s", text);
}

static float browse_clip_top(void)
{
    return g_view == VIEW_LIBRARIES ? BROWSE_CONTENT_TOP : GRID_CARD_TOP - 2;
}

static void browse_rect(float x, float y, float w, float h, u32 color, bool clip)
{
    float top = clip ? browse_clip_top() : 0, bottom = clip ? BROWSE_CONTENT_BOTTOM : 240;
    if (y < top) { h -= top - y; y = top; }
    if (y + h > bottom) h = bottom - y;
    if (h > 0) C2D_DrawRectSolid(x, y, 0, w, h, color);
}

static void browse_art(const MediaItem *item, BrowseArtKind kind, float x, float y, float w, float h, bool clip)
{
    BrowseImage *entry = browse_find_image(item, kind, false);
    float top = clip ? browse_clip_top() : 0, bottom = clip ? BROWSE_CONTENT_BOTTOM : 240;
    if (y >= bottom || y + h <= top) return;
    browse_rect(x, y, w, h, 0xFF292323, clip);
    if (entry && entry->ready) {
        Tex3DS_SubTexture sub = entry->sub;
        float sy = y < top ? top : y, ey = y + h > bottom ? bottom : y + h;
        float range = sub.top - sub.bottom;
        sub.top -= (sy - y) / h * range;
        sub.bottom += (y + h - ey) / h * range;
        sub.height = (u16)(entry->sub.height * (ey - sy) / h);
        if (!sub.height) return;
        C2D_Image image = { &entry->tex, &sub };
        C2D_DrawImageAt(image, x, sy, 0, NULL, w / sub.width, (ey - sy) / sub.height);
    } else if (kind == BROWSE_ART_SQUARE) {
        unsigned hue = 0;
        for (const char *p = item->id; *p; ++p) hue = hue * 33 + (unsigned char)*p;
        u32 color = 0xFF000000 | ((40 + hue % 55) << 16) | ((30 + (hue >> 8) % 45) << 8) | (55 + (hue >> 16) % 55);
        browse_rect(x, y, w, h, color, clip);
        if (y >= top && y + h <= bottom) {
            C2D_DrawCircleSolid(x + w / 2, y + h / 2, 0, w * 0.34f, 0xFF252020);
            C2D_DrawCircleSolid(x + w / 2, y + h / 2, 0, w * 0.12f, COL_SECONDARY);
            C2D_DrawCircleSolid(x + w / 2, y + h / 2, 0, w * 0.035f, color);
        }
    } else if (y + h / 2 - 8 >= top && y + h / 2 + 8 <= bottom) {
        browse_label(x + 5, y + h / 2 - 8, w - 10, 0.5f, COL_MUTED,
                     entry && entry->failed ? "No image" : display_item_kind(item));
    }
}

static void browse_card(MediaItem *item, bool poster, float x, float y, bool focused)
{
    float w = poster ? 88 : 184, h = poster ? 122 : 104;
    if (music_is_item(item)) h = w;
    if (focused) browse_rect(x - 2, y - 2, w + 4, h + 4, COL_PRIMARY, true);
    browse_art(item, music_is_item(item) ? BROWSE_ART_SQUARE : poster ? BROWSE_ART_POSTER : BROWSE_ART_LANDSCAPE, x, y, w, h, true);
    if (item->is_favorite && y >= browse_clip_top() && y + 18 < BROWSE_CONTENT_BOTTOM) {
        C2D_DrawRectSolid(x + w - 20, y + 2, 0, 18, 18, 0xE0202020);
        draw_text(x + w - 18, y + 2, 0.5f, COL_SECONDARY, "\xE2\x99\xA5");
    }
    if (item->position_ticks && item->runtime_ticks && !item->played) {
        double progress = (double)item->position_ticks / item->runtime_ticks;
        if (progress > 1) progress = 1;
        browse_rect(x, y + h - 4, w, 4, 0xFF333333, true);
        browse_rect(x, y + h - 4, (float)(w * progress), 4, COL_PRIMARY, true);
    }
    const char *name = item->series_name[0] && !strcmp(item->type, "Episode") ? item->series_name : item->name;
    if (y + h + 3 >= browse_clip_top() && y + h + 18 <= BROWSE_CONTENT_BOTTOM)
        browse_label(x, y + h + 3, w, 0.5f, COL_WHITE, name);
    /* Episode/year details are easier to read in the larger bottom preview. */
}

static void browse_episode_meta(const MediaItem *item, char *out, size_t size)
{
    browse_format_meta(item, out, size);
    const char *state = item->played ? "  /  Watched" : media_resume_position(item) ? "  /  In progress" : "";
    if (size) strncat(out, state, size - strlen(out) - 1);
}

static void draw_episode_row(MediaItem *item, float y, bool focused)
{
    browse_rect(8, y, 384, 72, focused ? 0xFF302B23 : 0xFF201D1D, true);
    if (focused) browse_rect(8, y, 3, 72, COL_PRIMARY, true);
    browse_art(item, BROWSE_ART_EPISODE, 14, y + 4, 112, 63, true);
    if (media_resume_position(item) && item->runtime_ticks) {
        float progress = (double)item->position_ticks / item->runtime_ticks;
        browse_rect(14, y + 63, 112, 4, 0xFF333333, true);
        browse_rect(14, y + 63, 112 * progress, 4, COL_PRIMARY, true);
    }
    if (item->is_favorite && y + 6 >= browse_clip_top() && y + 24 <= BROWSE_CONTENT_BOTTOM) {
        C2D_DrawRectSolid(106, y + 6, 0, 18, 18, 0xE0202020);
        draw_text(107, y + 6, 0.5f, COL_SECONDARY, "\xE2\x99\xA5");
    }
    draw_text_lines_clipped(136, y + 3, 0.5f, 246, COL_WHITE, item->name, 2, 17,
                            browse_clip_top(), BROWSE_CONTENT_BOTTOM);
    char meta[128];
    browse_episode_meta(item, meta, sizeof(meta));
    if (y + 39 >= browse_clip_top() && y + 54 <= BROWSE_CONTENT_BOTTOM)
        browse_label(136, y + 39, 246, 0.5f, focused ? COL_PRIMARY : COL_MUTED, meta);
    if (y + 55 >= browse_clip_top() && y + 70 <= BROWSE_CONTENT_BOTTOM)
        browse_label(136, y + 55, 246, 0.5f, COL_MUTED,
                     item->overview[0] ? item->overview : "No summary available");
}

static void draw_episode_preview(const MediaItem *item)
{
    browse_art(item, BROWSE_ART_EPISODE, 10, 8, 142, 80, false);
    draw_text_lines(164, 9, 0.55f, 146, COL_WHITE,
                    item->series_name[0] ? item->series_name : g_screen_title, 2, 18);
    char meta[128];
    browse_format_meta(item, meta, sizeof(meta));
    browse_label(164, 49, 146, 0.5f, COL_MUTED, meta);
    u64 resume = media_resume_position(item);
    char state[64];
    if (resume) {
        char at[32];
        format_playback_time(resume, at, sizeof(at));
        snprintf(state, sizeof(state), "Resume %s", at);
    } else copy_safe(state, sizeof(state), item->played ? "Watched" : "Not watched");
    browse_label(164, 70, 146, 0.5f, COL_PRIMARY, state);
    draw_text_lines(10, 98, 0.6f, 300, COL_WHITE, item->name, 2, 20);
    draw_text_lines(10, 141, 0.5f, 300, COL_MUTED,
                    item->overview[0] ? item->overview : "No summary available for this episode.", 2, 17);
}

static const char *browse_tab_name(int tab)
{
    return tab == 0 ? "Home" : tab == 1 ? "Favorites" : g_libraries[tab - 2].name;
}

static float browse_tab_width(int tab)
{
    float width = text_width_for_scale(browse_tab_name(tab), 0.5f) + 22;
    if (width < 62) width = 62;
    if (width > 360) width = 360;
    return width;
}

static void draw_browse_tabs(void)
{
    int focus = g_tab_focus ? g_nav_tab : g_active_tab;
    if (g_tab_first > focus || g_tab_first < 0) g_tab_first = focus;
    float needed = 0;
    for (int i = g_tab_first; i <= focus; ++i) needed += browse_tab_width(i);
    while (needed > 372 && g_tab_first < focus) needed -= browse_tab_width(g_tab_first++);
    C2D_DrawRectSolid(0, 0, 0, 400, 34, COL_BG);
    C2D_DrawRectSolid(0, 33, 0, 400, 1, 0xFF404040);
    if (g_tab_first) draw_text(2, 8, 0.5f, COL_WHITE, "<");
    float x = 14;
    int i = g_tab_first;
    for (; i < g_library_count + 2; ++i) {
        float width = browse_tab_width(i);
        if (x + width > 386) break;
        if (g_tab_focus && i == g_nav_tab) C2D_DrawRectSolid(x, 3, 0, width - 3, 27, 0xFF383028);
        browse_label(x + 8, 8, width - 18, 0.5f, i == focus ? COL_PRIMARY : COL_WHITE, browse_tab_name(i));
        if (i == g_active_tab) C2D_DrawRectSolid(x + 6, 30, 0, width - 15, 3, COL_PRIMARY);
        x += width;
    }
    if (i < g_library_count + 2) draw_text(389, 8, 0.5f, COL_WHITE, ">");
}

static void home_row_title(int row, char *out, size_t size)
{
    if (!row) copy_safe(out, size, "My Media");
    else if (row == 1) copy_safe(out, size, "Continue Watching");
    else if (row == 2) copy_safe(out, size, "Next Up");
    else snprintf(out, size, "Recently Added in %s", g_libraries[row - 3].name);
}

static void draw_browse_scroll_hint(void)
{
    float top = browse_clip_top(), visible = BROWSE_CONTENT_BOTTOM - top;
    float total = g_view == VIEW_LIBRARIES ? home_y(g_library_count + 3) : music_track_list() ?
        (g_item_count > 3 ? g_item_count - 3 : 0) * 48 + visible : music_browse_view() ?
        (g_item_count ? (g_item_count - 1) / 3 : 0) * 150 + visible : browse_is_episode_list() ?
        (g_item_count > EPISODE_VISIBLE_ROWS ? g_item_count - EPISODE_VISIBLE_ROWS : 0) * EPISODE_ROW_HEIGHT + visible :
        (g_item_count ? (g_item_count - 1) / GRID_COLUMNS : 0) * GRID_ROW_HEIGHT + visible;
    if (total > visible) {
        float thumb = visible * visible / total;
        if (thumb < 16) thumb = 16;
        float position = g_browse_scroll.value / (total - visible);
        if (position < 0) position = 0;
        if (position > 1) position = 1;
        C2D_DrawRectSolid(396, top, 0, 3, visible, 0xFF383030);
        C2D_DrawRectSolid(396, top + position * (visible - thumb), 0, 3, thumb, COL_PRIMARY);
    }
    char hint[256];
    if (g_music_mode_focus && music_browse_view()) copy_safe(hint, sizeof(hint), "Left/Right: Choose   A: Open   Down: Browse");
    else if (g_tab_focus) copy_safe(hint, sizeof(hint), "Down: Browse   L/R: Libraries");
    else if (g_view == VIEW_LIBRARIES) {
        if (g_home_row < g_library_count + 2) {
            char next[192];
            home_row_title(g_home_row + 1, next, sizeof(next));
            snprintf(hint, sizeof(hint), "Down: %s", next);
        } else copy_safe(hint, sizeof(hint), "Up: Previous rows");
    } else if (music_browse_view()) {
        copy_safe(hint, sizeof(hint), music_track_list() ? "A: Play   Up/Down: Songs   B: Back" : "A: Open   Up: Music views   B: Back");
    } else if (browse_is_episode_list()) {
        copy_safe(hint, sizeof(hint), !g_item_count ? "B: Back   L/R: Libraries" :
                  g_selected + 1 < g_item_count ? "Down: More episodes   B: Back" : "Up: Episodes   B: Back");
    } else if (g_scroll + GRID_COLUMNS < g_item_count) copy_safe(hint, sizeof(hint), "Down: More titles   B: Back");
    else if (g_scroll) copy_safe(hint, sizeof(hint), "Up: Previous titles   B: Back");
    else copy_safe(hint, sizeof(hint), "A: Open   B: Back   L/R: Libraries");
    C2D_DrawRectSolid(0, 220, 0, 400, 20, COL_BG);
    C2D_DrawRectSolid(0, 220, 0, 400, 1, 0xFF383030);
    browse_label(10, 222, 380, 0.5f, COL_PRIMARY, hint);
}

static void draw_browse_top(void)
{
    C2D_DrawRectSolid(0, 0, 0, 400, 240, COL_BG);
    if (g_view == VIEW_LIBRARIES) {
        for (int r = 0; r < g_library_count + 3; ++r) {
            float y = BROWSE_CONTENT_TOP + home_y(r) - g_browse_scroll.value;
            if (y >= BROWSE_CONTENT_BOTTOM || y + home_height(r) < BROWSE_CONTENT_TOP) continue;
            char title[192];
            home_row_title(r, title, sizeof(title));
            if (y >= BROWSE_CONTENT_TOP && y + 18 < BROWSE_CONTENT_BOTTOM)
                browse_label(10, y, 315, 0.55f, COL_WHITE, title);
            int count, *selected, *first;
            MediaItem *items = home_items(r, &count, &selected, &first);
            if (!count && y + 24 >= BROWSE_CONTENT_TOP && y + 40 < BROWSE_CONTENT_BOTTOM) {
                const char *empty = r == 1 ? "No unfinished videos" : r == 2 ? "No next episodes yet" : "No items yet";
                if (r && !g_home_rows[r - 1].loaded) empty = "Loading...";
                if (r && g_home_rows[r - 1].failed) empty = "Could not load. X: retry";
                browse_label(14, y + 24, 370, 0.5f, COL_MUTED, empty);
            }
            bool poster = r >= 3;
            int columns = poster ? GRID_COLUMNS : HOME_LANDSCAPE_COLUMNS;
            if (y >= BROWSE_CONTENT_TOP && y + 18 < BROWSE_CONTENT_BOTTOM && count > columns) {
                char counter[32];
                snprintf(counter, sizeof(counter), "%d/%d", *selected + 1, count);
                browse_label(332, y + 1, 60, 0.5f, COL_MUTED, counter);
            }
            for (int i = *first; i < count && i < *first + columns; ++i)
                browse_card(&items[i], poster, 10 + (i - *first) * (poster ? 97 : 196), y + HOME_CARD_TOP,
                            !g_tab_focus && r == g_home_row && i == *selected);
        }
    } else if (music_browse_view()) {
        draw_music_browse();
    } else {
        browse_label(10, 40, 308, 0.55f, COL_WHITE, g_screen_title);
        draw_text(325, 41, 0.45f, COL_MUTED, "%d/%d", g_item_count ? g_selected + 1 : 0, g_item_count);
        if (!g_item_count) draw_text(14, 86, 0.55f, COL_MUTED, "No items found");
        if (browse_is_episode_list()) {
            int first = (int)(g_browse_scroll.value / EPISODE_ROW_HEIGHT);
            for (int i = first; i < g_item_count && i < first + 4; ++i) {
                float y = GRID_CARD_TOP + i * EPISODE_ROW_HEIGHT - g_browse_scroll.value;
                if (y >= BROWSE_CONTENT_BOTTOM) break;
                draw_episode_row(&g_items[i], y, !g_tab_focus && i == g_selected);
            }
        } else {
            int first_row = (int)(g_browse_scroll.value / GRID_ROW_HEIGHT);
            for (int i = first_row * GRID_COLUMNS; i < g_item_count && i < (first_row + 3) * GRID_COLUMNS; ++i) {
                float y = GRID_CARD_TOP + (i / GRID_COLUMNS) * GRID_ROW_HEIGHT - g_browse_scroll.value;
                if (y >= BROWSE_CONTENT_BOTTOM) break;
                browse_card(&g_items[i], true, 10 + (i % GRID_COLUMNS) * 97,
                            y, !g_tab_focus && i == g_selected);
            }
        }
    }
    draw_browse_tabs();
    draw_browse_scroll_hint();
}

static void draw_browse_bottom(void)
{
    MediaItem *item = browse_focused();
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_BG);
    if (item) {
        bool library = g_tab_focus || (g_view == VIEW_LIBRARIES && !g_home_row);
        bool episode = browse_is_episode_list() && !library;
        if (!library && music_is_item(item)) draw_music_preview(item);
        else if (episode) draw_episode_preview(item);
        else {
            bool poster = browse_item_prefers_poster_art(item);
            float tx = poster ? 104 : 12, ty = poster ? 10 : 92, tw = poster ? 204 : 296;
            browse_art(item, poster ? BROWSE_ART_POSTER : BROWSE_ART_LANDSCAPE,
                        poster ? 10 : 89, 8, poster ? 84 : 142, poster ? 120 : 80, false);
            const char *title = item->series_name[0] && !strcmp(item->type, "Episode") ? item->series_name : item->name;
            draw_text_lines(tx, ty, 0.6f, tw, COL_WHITE, title, 2, 20);
            char meta[256];
            browse_format_meta(item, meta, sizeof(meta));
            if (library) copy_safe(meta, sizeof(meta), "Library");
            else if (item->series_name[0]) snprintf(meta, sizeof(meta), "S%d:E%d - %s", item->parent_index_number, item->index_number, item->name);
            browse_label(tx, ty + 42, tw, 0.5f, COL_MUTED, meta);
            if (poster && item->overview[0]) draw_text_lines(tx, ty + 63, 0.5f, tw, COL_MUTED, item->overview, 5, 17);
        }
        u64 resume = media_resume_position(item);
        if (resume && !episode && !music_is_item(item)) {
            char at[32];
            format_playback_time(resume, at, sizeof(at));
            draw_text(12, 162, 0.5f, COL_PRIMARY, "Resume at %s", at);
        }
        C2D_DrawRectSolid(10, BROWSE_ACTION_TOP, 0, library ? 300 : 145, 27, COL_PRIMARY_DARK);
        draw_text_centered(library ? 160 : 82, BROWSE_ACTION_TOP + 5, 0.5f, COL_WHITE,
                           "%s", library ? "Open library" : is_playable(item) ? (resume && !music_is_audio(item) ? "Resume" : "Play") : "Open");
        if (!library) {
            C2D_DrawRectSolid(165, BROWSE_ACTION_TOP, 0, 145, 27, 0xFF30232D);
            draw_text_centered(237, BROWSE_ACTION_TOP + 5, 0.5f, item->is_favorite ? COL_SECONDARY : COL_WHITE,
                               "%s", item->is_favorite ? "Unfavorite" : "Favorite");
        }
    } else {
        const char *title = g_music_mode_focus ? music_mode_name(g_music_nav_mode) : g_tab_focus ? browse_tab_name(g_nav_tab) : "Your Jellyfin library";
        draw_text_centered(160, 54, 0.65f, COL_WHITE, "%s", title);
        draw_text_lines(20, 93, 0.5f, 280, COL_MUTED,
                        (g_tab_focus || g_music_mode_focus) ? "Press A or tap Open." : "Choose a library or title on the top screen.", 2, 18);
        if (g_tab_focus || g_music_mode_focus) {
            C2D_DrawRectSolid(10, BROWSE_ACTION_TOP, 0, 300, 27, COL_PRIMARY_DARK);
            draw_text_centered(160, BROWSE_ACTION_TOP + 5, 0.5f, COL_WHITE, "Open");
        }
    }
    if (osGetTime() < g_browse_notice_until) {
        C2D_DrawRectSolid(0, 154, 0, 320, 27, COL_PAPER);
        browse_label(10, 161, 300, 0.5f, COL_WHITE, g_status);
    }
    C2D_DrawRectSolid(10, BROWSE_MENU_TOP, 0, 145, 26, COL_PAPER);
    C2D_DrawRectSolid(165, BROWSE_MENU_TOP, 0, 145, 26, COL_PAPER);
    draw_text_centered(82, BROWSE_MENU_TOP + 4, 0.5f, COL_WHITE, "Server setup");
    draw_text_centered(237, BROWSE_MENU_TOP + 4, 0.5f, COL_WHITE, "Settings");
}

static void browse_activate_tab(void)
{
    g_music_mode_focus = false;
    int target = g_nav_tab;
    browse_cancel_request();
    bool ok = true;
    if (!target) {
        g_view = VIEW_LIBRARIES;
        g_stack_depth = 0;
        g_current_parent_id[0] = g_current_parent_type[0] = g_current_parent_series_id[0] = 0;
        copy_safe(g_screen_title, sizeof(g_screen_title), "Home");
    } else {
        if (target == 1) ok = load_items_for_parent(FAVORITES_PARENT_ID, "Favorites");
        else ok = load_items_for_media_item(&g_libraries[target - 2]);
        if (ok) g_stack_depth = 0;
    }
    if (ok) {
        g_active_tab = g_nav_tab = target;
        g_tab_focus = false;
    } else g_browse_notice_until = osGetTime() + 4000;
}

static void browse_activate_item(void)
{
    if (g_tab_focus) { browse_activate_tab(); return; }
    if (g_music_mode_focus) { music_activate_mode(); return; }
    MediaItem *item = browse_focused();
    if (!item) return;
    MediaItem copy = *item;
    browse_cancel_request();
    if (g_view == VIEW_LIBRARIES && !g_home_row) {
        g_nav_tab = g_library_selected + 2;
        browse_activate_tab();
    } else if (music_is_audio(&copy)) {
        music_start(&copy);
    } else if (is_playable(&copy)) {
        g_current = copy;
        g_return_view = g_view;
        probe_playback();
    } else {
        push_nav(g_view == VIEW_SEARCH ? SEARCH_NAV_PARENT_ID : g_view == VIEW_LIBRARIES ? "" : g_current_parent_id,
                 g_screen_title);
        if (!load_items_for_media_item(&copy)) g_browse_notice_until = osGetTime() + 4000;
    }
}

static u32 browse_repeat(u32 down, u32 held)
{
    u32 mask = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
    u32 key = down & mask;
    if (key) { g_browse_repeat_key = key; g_browse_repeat_at = osGetTime() + 280; return key; }
    if (!(held & g_browse_repeat_key)) { g_browse_repeat_key = 0; return 0; }
    if (g_browse_repeat_key && osGetTime() >= g_browse_repeat_at) {
        g_browse_repeat_at = osGetTime() + ((g_browse_repeat_key & (KEY_UP | KEY_DOWN)) ? BROWSE_SCROLL_MS : 110);
        return g_browse_repeat_key;
    }
    return 0;
}

static void browse_move(u32 keys)
{
    int dx = (keys & KEY_RIGHT) ? 1 : (keys & KEY_LEFT) ? -1 : 0;
    int dy = (keys & KEY_DOWN) ? 1 : (keys & KEY_UP) ? -1 : 0;
    if (g_tab_focus) {
        g_nav_tab += dx;
        if (dy > 0) g_tab_focus = false;
    } else if (g_view == VIEW_LIBRARIES) {
        if (dy < 0 && !g_home_row) { g_tab_focus = true; g_nav_tab = g_active_tab; }
        else if (dy) g_home_row += dy;
        else {
            int count, *selected, *first;
            home_items(g_home_row, &count, &selected, &first);
            *selected += dx;
        }
    } else if (music_browse_view()) {
        if (g_music_mode_focus) {
            g_music_nav_mode = clamp_int(g_music_nav_mode + dx, 0, 2);
            if (dy > 0) g_music_mode_focus = false;
            if (dy < 0) { g_music_mode_focus = false; g_tab_focus = true; g_nav_tab = g_active_tab; }
        } else {
            int columns = music_track_list() ? 1 : 3;
            if (dy < 0 && g_selected < columns) {
                if (music_is_root_type(g_current_parent_type)) {
                    g_music_mode_focus = true;
                    g_music_nav_mode = music_mode_for_type(g_current_parent_type);
                } else { g_tab_focus = true; g_nav_tab = g_active_tab; }
            } else if (dy) g_selected += dy * columns;
            else if (dx) g_selected += dx;
        }
    } else if (browse_is_episode_list()) {
        if (dy < 0 && !g_selected) { g_tab_focus = true; g_nav_tab = g_active_tab; }
        else if (dy) g_selected += dy;
        else if (dx) g_selected += dx;
    } else {
        if (dy < 0 && g_selected < GRID_COLUMNS) { g_tab_focus = true; g_nav_tab = g_active_tab; }
        else if (dy) g_selected += dy * GRID_COLUMNS;
        else if (dx) g_selected += dx;
    }
    browse_clamp();
}

#include "music_ui.inc"

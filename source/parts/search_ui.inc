/* Global search. Every library is queried at the same time (several requests
 * share one curl multi handle) and results stream into shelves grouped by kind,
 * each card tagged with the library it came from. Results live in their own
 * storage, so backing out of a title returns to them instantly. */
#define SEARCH_MAX_RESULTS 384
#define SEARCH_MAX_JOBS (MAX_LIBRARIES * 2 + 2)
#define SEARCH_PARALLEL 4
#define SEARCH_TITLE_LIMIT 40
#define SEARCH_PART_LIMIT 24
#define SEARCH_HEADER_H 20
#define SEARCH_FIELDS "Overview,PrimaryImageAspectRatio,ChildCount,RecursiveItemCount"

enum {
    SG_MOVIES, SG_SHOWS, SG_EPISODES, SG_ARTISTS, SG_ALBUMS, SG_SONGS, SG_COLLECTIONS, SG_VIDEOS, SG_COUNT
};
static const char *const SEARCH_GROUP_NAMES[SG_COUNT] = {
    "Movies", "Shows", "Episodes", "Artists", "Albums", "Songs", "Collections", "Videos"
};

typedef struct {
    CURL *http;
    struct curl_slist *headers;
    CurlResponseBuffer response;
    char path[704];
    int library;
    bool started, done;
} SearchJob;

static MediaItem g_search_items[SEARCH_MAX_RESULTS];
static signed char g_search_library[SEARCH_MAX_RESULTS];
static unsigned char g_search_rank[SEARCH_MAX_RESULTS];
static int g_search_count;
static u16 g_search_groups[SG_COUNT][SEARCH_MAX_RESULTS];
static int g_search_group_count[SG_COUNT];
static int g_search_group_selected[SG_COUNT];
static int g_search_group_first[SG_COUNT];
static int g_search_focus = -1; /* group index, or -1 for the search bar */
static bool g_search_user_moved;
static SearchJob g_search_jobs[SEARCH_MAX_JOBS];
static int g_search_job_count, g_search_jobs_done, g_search_jobs_failed;
static CURLM *g_search_multi;
static MediaItem g_search_scratch[SEARCH_TITLE_LIMIT];

static bool search_running(void)
{
    return g_search_jobs_done < g_search_job_count;
}

static bool search_bar_focused(void)
{
    return g_search_focus < 0;
}

static int search_group_for(const MediaItem *item)
{
    const char *t = item->type;
    if (!strcmp(t, "Movie")) return SG_MOVIES;
    if (!strcmp(t, "Series")) return SG_SHOWS;
    if (!strcmp(t, "Episode")) return SG_EPISODES;
    if (!strcmp(t, "MusicArtist")) return SG_ARTISTS;
    if (!strcmp(t, "MusicAlbum")) return SG_ALBUMS;
    if (!strcmp(t, "Audio")) return SG_SONGS;
    if (!strcmp(t, "BoxSet")) return SG_COLLECTIONS;
    return SG_VIDEOS;
}

static const char *search_kind_label(const MediaItem *item)
{
    const char *t = item->type;
    if (!strcmp(t, "Movie")) return "Movie";
    if (!strcmp(t, "Series")) return "Show";
    if (!strcmp(t, "Episode")) return "Episode";
    if (!strcmp(t, "MusicArtist")) return "Artist";
    if (!strcmp(t, "MusicAlbum")) return "Album";
    if (!strcmp(t, "Audio")) return "Song";
    if (!strcmp(t, "BoxSet")) return "Collection";
    if (!strcmp(t, "MusicVideo")) return "Music video";
    return "Video";
}

static BrowseArtKind search_group_art(int group)
{
    if (group == SG_ARTISTS || group == SG_ALBUMS || group == SG_SONGS) return BROWSE_ART_SQUARE;
    if (group == SG_EPISODES || group == SG_VIDEOS) return BROWSE_ART_EPISODE;
    return BROWSE_ART_POSTER;
}

static void search_card_size(int group, float *w, float *h)
{
    BrowseArtKind kind = search_group_art(group);
    *w = kind == BROWSE_ART_EPISODE ? 120 : 70;
    *h = kind == BROWSE_ART_SQUARE ? 70 : kind == BROWSE_ART_EPISODE ? 68 : 100;
}

static int search_group_columns(int group)
{
    return search_group_art(group) == BROWSE_ART_EPISODE ? 3 : 5;
}

static float search_group_step(int group)
{
    return search_group_art(group) == BROWSE_ART_EPISODE ? 128 : 77;
}

static int search_group_height(int group)
{
    float w, h;
    search_card_size(group, &w, &h);
    return SEARCH_HEADER_H + (int)h + 40;
}

static float search_group_y(int group)
{
    float y = 0;
    for (int g = 0; g < group; ++g)
        if (g_search_group_count[g]) y += search_group_height(g);
    return y;
}

static float search_total_height(void)
{
    return search_group_y(SG_COUNT);
}

static int search_first_group(void)
{
    for (int g = 0; g < SG_COUNT; ++g)
        if (g_search_group_count[g]) return g;
    return -1;
}

static MediaItem *search_focused_item(void)
{
    if (g_search_focus < 0 || !g_search_group_count[g_search_focus]) return NULL;
    int sel = g_search_group_selected[g_search_focus];
    if (sel < 0 || sel >= g_search_group_count[g_search_focus]) return NULL;
    return &g_search_items[g_search_groups[g_search_focus][sel]];
}

static int search_focused_library(void)
{
    MediaItem *item = search_focused_item();
    return item ? g_search_library[item - g_search_items] : -1;
}

/* Lower is better: exact title, then prefix, then word prefix, then anything. */
static int search_rank(const char *name)
{
    size_t n = strlen(g_search_query);
    if (!n) return 3;
    if (!strcasecmp(name, g_search_query)) return 0;
    if (!strncasecmp(name, g_search_query, n)) return 1;
    for (const char *p = name; *p; ++p)
        if ((*p == ' ' || *p == '-' || *p == ':') && !strncasecmp(p + 1, g_search_query, n)) return 2;
    return 3;
}

static void search_insert(const MediaItem *item, int library)
{
    if (g_search_count >= SEARCH_MAX_RESULTS) return;
    for (int i = 0; i < g_search_count; ++i)
        if (g_search_items[i].id[0] == item->id[0] && !strcmp(g_search_items[i].id, item->id)) return;
    int idx = g_search_count++;
    g_search_items[idx] = *item;
    g_search_library[idx] = (signed char)library;
    g_search_rank[idx] = (unsigned char)search_rank(item->name);

    int group = search_group_for(item);
    int count = g_search_group_count[group];
    int pos = count;
    while (pos > 0 && g_search_rank[g_search_groups[group][pos - 1]] > g_search_rank[idx]) pos--;
    memmove(&g_search_groups[group][pos + 1], &g_search_groups[group][pos], (count - pos) * sizeof(u16));
    g_search_groups[group][pos] = (u16)idx;
    g_search_group_count[group] = count + 1;
    /* Keep the focused card under the cursor once the user starts browsing a row. */
    int *sel = &g_search_group_selected[group], *first = &g_search_group_first[group];
    if ((*sel || *first) && pos <= *sel) {
        (*sel)++;
        if (pos <= *first) (*first)++;
    }
    if (!g_search_user_moved) g_search_focus = search_first_group();
}

static void search_job_release(SearchJob *job)
{
    if (job->http) {
        if (g_search_multi) curl_multi_remove_handle(g_search_multi, job->http);
        curl_easy_cleanup(job->http);
    }
    curl_slist_free_all(job->headers);
    free(job->response.data);
    job->http = NULL;
    job->headers = NULL;
    memset(&job->response, 0, sizeof(job->response));
}

static void search_cancel(void)
{
    for (int i = 0; i < g_search_job_count; ++i) search_job_release(&g_search_jobs[i]);
    g_search_job_count = g_search_jobs_done = g_search_jobs_failed = 0;
}

static bool search_job_begin(SearchJob *job)
{
    job->started = true;
    if (!curl_http_runtime_init()) return false;
    if (!g_search_multi) g_search_multi = curl_multi_init();
    job->http = curl_easy_init();
    if (!g_search_multi || !job->http) return false;
    char url[1024], auth[384], header[448];
    build_url(url, sizeof(url), job->path);
    auth_header(auth, sizeof(auth), true);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    job->headers = curl_slist_append(NULL, header);
    curl_easy_setopt(job->http, CURLOPT_URL, url);
    curl_easy_setopt(job->http, CURLOPT_HTTPHEADER, job->headers);
    curl_easy_setopt(job->http, CURLOPT_USERAGENT, "3dJelly/" APP_VERSION);
    curl_easy_setopt(job->http, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(job->http, CURLOPT_WRITEDATA, &job->response);
    curl_easy_setopt(job->http, CURLOPT_PRIVATE, job);
    curl_easy_setopt(job->http, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(job->http, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(job->http, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(job->http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(job->http, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(job->http, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(job->http, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(job->http, CURLOPT_SSL_VERIFYHOST, 0L);
    return curl_multi_add_handle(g_search_multi, job->http) == CURLM_OK;
}

static void search_job_finish(SearchJob *job, bool ok)
{
    if (ok) {
        int count = 0;
        ok = parse_items(job->response.data, g_search_scratch, &count, SEARCH_TITLE_LIMIT, true, NULL);
        for (int i = 0; ok && i < count; ++i) search_insert(&g_search_scratch[i], job->library);
    }
    search_job_release(job);
    job->done = true;
    g_search_jobs_done++;
    if (!ok) g_search_jobs_failed++;
}

static void search_pump(void)
{
    int active = 0;
    for (int i = 0; i < g_search_job_count; ++i)
        if (g_search_jobs[i].started && !g_search_jobs[i].done) active++;
    for (int i = 0; i < g_search_job_count && active < SEARCH_PARALLEL; ++i) {
        SearchJob *job = &g_search_jobs[i];
        if (job->started) continue;
        if (search_job_begin(job)) active++;
        else search_job_finish(job, false);
    }
}

static void search_tick(void)
{
    if (!search_running() || !g_search_multi) return;
    int running = 0, remaining = 0;
    curl_multi_perform(g_search_multi, &running);
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(g_search_multi, &remaining))) {
        if (msg->msg != CURLMSG_DONE) continue;
        SearchJob *job = NULL;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&job);
        if (!job || job->done) continue;
        long status = 0;
        curl_easy_getinfo(job->http, CURLINFO_RESPONSE_CODE, &status);
        search_job_finish(job, msg->data.result == CURLE_OK && status == 200 && job->response.data);
    }
    search_pump();
    if (!search_running()) {
        if (g_search_count) set_status("%d results for \"%s\"", g_search_count, g_search_query);
        else if (g_search_jobs_failed) set_status("Search failed for \"%s\"", g_search_query);
        else set_status("No results for \"%s\"", g_search_query);
    }
}

static void search_add_job(int library, const char *types, int limit, bool artists)
{
    if (g_search_job_count >= SEARCH_MAX_JOBS) return;
    SearchJob *job = &g_search_jobs[g_search_job_count++];
    memset(job, 0, sizeof(*job));
    job->library = library;
    char term[192], parent[240] = "";
    url_encode(g_search_query, term, sizeof(term));
    if (library >= 0) {
        char id[200];
        url_encode(g_libraries[library].id, id, sizeof(id));
        snprintf(parent, sizeof(parent), "&ParentId=%s", id);
    }
    if (artists) {
        snprintf(job->path, sizeof(job->path),
                 "/Artists?UserId=%s&SearchTerm=%s%s&Limit=%d&EnableImageTypes=Primary&ImageTypeLimit=1"
                 "&EnableUserData=true&EnableTotalRecordCount=false&Fields=" SEARCH_FIELDS,
                 g_cfg.user_id, term, parent, limit);
    } else {
        snprintf(job->path, sizeof(job->path),
                 "/Users/%s/Items?SearchTerm=%s%s&Recursive=true&Limit=%d&IncludeItemTypes=%s"
                 "&EnableImageTypes=Primary,Thumb&ImageTypeLimit=1&EnableUserData=true&EnableTotalRecordCount=false"
                 "&IsMissing=false&Fields=" SEARCH_FIELDS,
                 g_cfg.user_id, term, parent, limit, types);
    }
}

static void search_start(const char *query)
{
    search_cancel();
    browse_cancel_request();
    copy_safe(g_search_query, sizeof(g_search_query), query);
    g_search_count = 0;
    memset(g_search_group_count, 0, sizeof(g_search_group_count));
    memset(g_search_group_selected, 0, sizeof(g_search_group_selected));
    memset(g_search_group_first, 0, sizeof(g_search_group_first));
    g_search_focus = -1;
    g_search_user_moved = false;

    bool any = false;
    for (int i = 0; i < g_library_count && i < 127; ++i) {
        const char *ct = g_libraries[i].collection_type;
        if (!strcmp(ct, "playlists") || !strcmp(ct, "livetv") || !strcmp(ct, "books") || !strcmp(ct, "photos")) continue;
        any = true;
        if (!strcmp(ct, "movies")) search_add_job(i, "Movie,Video", SEARCH_TITLE_LIMIT, false);
        else if (!strcmp(ct, "tvshows")) {
            search_add_job(i, "Series", SEARCH_TITLE_LIMIT, false);
            search_add_job(i, "Episode", SEARCH_PART_LIMIT, false);
        } else if (!strcmp(ct, "music")) {
            search_add_job(i, "MusicAlbum", SEARCH_PART_LIMIT, false);
            search_add_job(i, "Audio", SEARCH_TITLE_LIMIT, false);
            search_add_job(i, NULL, SEARCH_PART_LIMIT, true);
        } else if (!strcmp(ct, "musicvideos")) search_add_job(i, "MusicVideo,Video", SEARCH_TITLE_LIMIT, false);
        else if (!strcmp(ct, "homevideos")) search_add_job(i, "Video", SEARCH_TITLE_LIMIT, false);
        else if (!strcmp(ct, "boxsets")) search_add_job(i, "BoxSet", SEARCH_PART_LIMIT, false);
        else {
            search_add_job(i, "Movie,Series,Video,MusicVideo,MusicAlbum", SEARCH_TITLE_LIMIT, false);
            search_add_job(i, "Episode,Audio", SEARCH_PART_LIMIT, false);
        }
    }
    if (!any) {
        search_add_job(-1, "Movie,Series,BoxSet,Video,MusicVideo,MusicAlbum", SEARCH_TITLE_LIMIT, false);
        search_add_job(-1, "Episode,Audio", SEARCH_PART_LIMIT, false);
    }

    snprintf(g_screen_title, sizeof(g_screen_title), "Search: %s", g_search_query);
    g_current_parent_id[0] = g_current_parent_type[0] = g_current_parent_series_id[0] = 0;
    g_stack_depth = 0;
    g_tab_focus = false;
    g_music_mode_focus = false;
    g_view = VIEW_SEARCH;
    set_status("Searching for \"%s\"...", g_search_query);
    search_pump();
}

/* Called when backing out of a title that was opened from the results. */
static void search_restore_view(void)
{
    g_view = VIEW_SEARCH;
    snprintf(g_screen_title, sizeof(g_screen_title), "Search: %s", g_search_query);
    g_current_parent_id[0] = g_current_parent_type[0] = g_current_parent_series_id[0] = 0;
    g_tab_focus = false;
}

static void search_clamp(void)
{
    if (g_search_focus >= 0 && !g_search_group_count[g_search_focus]) g_search_focus = search_first_group();
    for (int g = 0; g < SG_COUNT; ++g) {
        int count = g_search_group_count[g], cols = search_group_columns(g);
        int *sel = &g_search_group_selected[g], *first = &g_search_group_first[g];
        if (*sel >= count) *sel = count ? count - 1 : 0;
        if (*sel < 0) *sel = 0;
        if (*first > *sel) *first = *sel;
        if (*sel >= *first + cols) *first = *sel - cols + 1;
        if (*first > count - cols) *first = count - cols;
        if (*first < 0) *first = 0;
    }
}

static float search_scroll_target(void)
{
    if (g_search_focus < 0) return 0;
    float visible = BROWSE_CONTENT_BOTTOM - BROWSE_CONTENT_TOP;
    float max = search_total_height() - visible;
    float y = search_group_y(g_search_focus);
    if (y > max) y = max;
    return y < 0 ? 0 : y;
}

static void search_move(int dx, int dy)
{
    if (dx || dy) g_search_user_moved = true;
    if (g_search_focus < 0) {
        if (dy > 0) g_search_focus = search_first_group();
        return;
    }
    if (dy < 0) {
        int g = g_search_focus - 1;
        while (g >= 0 && !g_search_group_count[g]) g--;
        g_search_focus = g;
    } else if (dy > 0) {
        for (int g = g_search_focus + 1; g < SG_COUNT; ++g)
            if (g_search_group_count[g]) { g_search_focus = g; break; }
    } else if (dx) {
        g_search_group_selected[g_search_focus] += dx;
    }
    search_clamp();
}

/* Songs play as a queue made from every song result. */
static void search_play_song(const MediaItem *song)
{
    MediaItem copy = *song;
    g_item_count = 0;
    for (int i = 0; i < g_search_group_count[SG_SONGS] && g_item_count < MAX_ITEMS; ++i)
        g_items[g_item_count++] = g_search_items[g_search_groups[SG_SONGS][i]];
    music_start(&copy);
}

static bool search_request_art(void)
{
    float scroll = g_browse_scroll.value;
    for (int g = 0; g < SG_COUNT; ++g) {
        if (!g_search_group_count[g]) continue;
        float y = BROWSE_CONTENT_TOP + search_group_y(g) - scroll;
        if (y + search_group_height(g) < BROWSE_CONTENT_TOP || y >= BROWSE_CONTENT_BOTTOM) continue;
        int first = g_search_group_first[g], cols = search_group_columns(g);
        for (int i = first; i < g_search_group_count[g] && i < first + cols; ++i)
            if (browse_request_art(&g_search_items[g_search_groups[g][i]], search_group_art(g))) return true;
    }
    return false;
}

/* ---------- drawing ---------- */

static void search_label_centered(float cx, float y, float width, float scale, u32 color, const char *label)
{
    char text[256];
    copy_safe(text, sizeof(text), label);
    trim_text_to_width(text, sizeof(text), scale, width, true);
    draw_text_centered(cx, y, scale, color, "%s", text);
}

static float search_pill(float x, float y, float max_w, float scale, u32 bg, u32 fg, const char *label)
{
    char text[96];
    copy_safe(text, sizeof(text), label);
    trim_text_to_width(text, sizeof(text), scale, max_w - 10, true);
    float w = text_width_for_scale(text, scale) + 10;
    C2D_DrawRectSolid(x, y, 0, w, scale * 30 + 2, bg);
    draw_text(x + 5, y + 1, scale, fg, "%s", text);
    return w;
}

/* The magnifier is rasterised once per size into an anti-aliased alpha texture
 * (16x supersampled), then tinted. Plain citro2d shapes have no anti-aliasing
 * and look jagged at icon sizes. */
#define SEARCH_ICON_SLOTS 3
typedef struct { int size; bool ready; C3D_Tex tex; Tex3DS_SubTexture sub; } SearchIcon;
static SearchIcon g_search_icons[SEARCH_ICON_SLOTS];

static float search_icon_coverage(float x, float y)
{
    /* Unit-square geometry: ring plus a round-capped handle. */
    const float cx = 0.41f, cy = 0.41f, r = 0.29f, half = 0.065f;
    float dx = x - cx, dy = y - cy;
    float d = sqrtf(dx * dx + dy * dy);
    if (fabsf(d - r) <= half) return 1;
    const float ax = cx + r * 0.7071f, ay = cy + r * 0.7071f, bx = 0.88f, by = 0.88f, hw = 0.085f;
    float vx = bx - ax, vy = by - ay, t = ((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float px = x - (ax + vx * t), py = y - (ay + vy * t);
    return px * px + py * py <= hw * hw ? 1 : 0;
}

static SearchIcon *search_icon_get(int size)
{
    if (size < 8) size = 8;
    if (size > 64) size = 64;
    for (int i = 0; i < SEARCH_ICON_SLOTS; ++i)
        if (g_search_icons[i].ready && g_search_icons[i].size == size) return &g_search_icons[i];
    SearchIcon *slot = NULL;
    for (int i = 0; i < SEARCH_ICON_SLOTS && !slot; ++i)
        if (!g_search_icons[i].ready) slot = &g_search_icons[i];
    if (!slot) { slot = &g_search_icons[0]; C3D_TexDelete(&slot->tex); slot->ready = false; }
    u32 *linear = calloc(64 * 64, sizeof(u32));
    u32 *tiled = linearAlloc(64 * 64 * sizeof(u32));
    if (!linear || !tiled || !C3D_TexInit(&slot->tex, 64, 64, GPU_RGBA8)) {
        free(linear);
        if (tiled) linearFree(tiled);
        return NULL;
    }
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    hits += search_icon_coverage((x + (sx + 0.5f) / 4) / size, (y + (sy + 0.5f) / 4) / size) > 0;
            linear[y * 64 + x] = 0xFFFFFF00u | (u32)(hits * 255 / 16);
        }
    int write = 0;
    for (int ty = 0; ty < 64; ty += 8)
        for (int tx = 0; tx < 64; tx += 8)
            for (int i = 0; i < 64; ++i) {
                int idx = BROWSE_TILE_REMAP[i];
                tiled[write++] = linear[(ty + (idx >> 3)) * 64 + tx + (idx & 7)];
            }
    C3D_TexSetFilter(&slot->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&slot->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexUpload(&slot->tex, tiled);
    C3D_TexFlush(&slot->tex);
    free(linear);
    linearFree(tiled);
    slot->sub = (Tex3DS_SubTexture){ .width = size, .height = size, .left = 0,
        .right = size / 64.0f, .top = 1, .bottom = 1 - size / 64.0f };
    slot->size = size;
    slot->ready = true;
    return slot;
}

static void search_draw_icon(float cx, float cy, float size, u32 color)
{
    SearchIcon *icon = search_icon_get((int)(size + 0.5f));
    if (!icon) return;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, color, 1.0f);
    C2D_Image image = { &icon->tex, &icon->sub };
    /* Pixel-aligned so the pre-filtered edges stay crisp. */
    C2D_DrawImageAt(image, floorf(cx - icon->size / 2.0f), floorf(cy - icon->size / 2.0f), 0, &tint, 1, 1);
}

static void search_draw_check(float cx, float cy)
{
    C2D_DrawCircleSolid(cx, cy, 0, 6.5f, COL_PRIMARY);
    C2D_DrawLine(cx - 3, cy, COL_WHITE, cx - 0.8f, cy + 2.5f, COL_WHITE, 1.6f, 0);
    C2D_DrawLine(cx - 0.8f, cy + 2.5f, COL_WHITE, cx + 3.2f, cy - 2.4f, COL_WHITE, 1.6f, 0);
}

static void search_draw_card(int idx, int group, float x, float y, bool focused)
{
    MediaItem *item = &g_search_items[idx];
    float w, h, top = BROWSE_CONTENT_TOP, bottom = BROWSE_CONTENT_BOTTOM;
    search_card_size(group, &w, &h);
    if (focused) browse_rect(x - 2, y - 2, w + 4, h + 4, COL_PRIMARY, true);
    browse_art(item, search_group_art(group), x, y, w, h, true);
    if (y >= top && y + 16 <= bottom) {
        if (item->played) search_draw_check(x + w - 9, y + 9);
        else if (item->is_favorite) {
            C2D_DrawRectSolid(x + w - 18, y + 2, 0, 16, 16, 0xE0202020);
            draw_text(x + w - 17, y + 1, 0.45f, COL_SECONDARY, "\xE2\x99\xA5");
        }
    }
    if (item->position_ticks && item->runtime_ticks && !item->played) {
        double progress = (double)item->position_ticks / item->runtime_ticks;
        if (progress > 1) progress = 1;
        browse_rect(x, y + h - 3, w, 3, 0xFF333333, true);
        browse_rect(x, y + h - 3, (float)(w * progress), 3, COL_PRIMARY, true);
    }
    const char *name = !strcmp(item->type, "Episode") && item->series_name[0] ? item->series_name : item->name;
    if (y + h + 2 >= top && y + h + 16 <= bottom)
        search_label_centered(x + w / 2, y + h + 2, w + 4, 0.45f, focused ? COL_PRIMARY : COL_WHITE, name);
    int lib = g_search_library[idx];
    const char *chip = lib >= 0 && lib < g_library_count ? g_libraries[lib].name : search_kind_label(item);
    if (y + h + 18 >= top && y + h + 32 <= bottom) {
        char text[96];
        copy_safe(text, sizeof(text), chip);
        trim_text_to_width(text, sizeof(text), 0.4f, w - 8, true);
        float cw = text_width_for_scale(text, 0.4f) + 10;
        C2D_DrawRectSolid(x + (w - cw) / 2, y + h + 18, 0, cw, 13, focused ? 0xFF4A3A26 : 0xFF333030);
        draw_text_centered(x + w / 2, y + h + 18, 0.4f, focused ? COL_WHITE : COL_MUTED, "%s", text);
    }
}

static void search_draw_bar(void)
{
    bool focused = g_search_focus < 0;
    C2D_DrawRectSolid(0, 0, 0, 400, 36, COL_BG);
    if (focused) C2D_DrawRectSolid(9, 4, 0, 382, 26, COL_PRIMARY);
    C2D_DrawRectSolid(10, 5, 0, 380, 24, focused ? 0xFF2E2A26 : 0xFF262222);
    char count[64];
    if (search_running()) {
        static const char *const dots[] = {"", ".", "..", "..."};
        snprintf(count, sizeof(count), "Searching%s", dots[(osGetTime() / 300) % 4]);
    } else snprintf(count, sizeof(count), "%d result%s", g_search_count, g_search_count == 1 ? "" : "s");
    float cw = text_width_for_scale(count, 0.45f);
    draw_text(382 - cw, 9, 0.45f, COL_MUTED, "%s", count);
    browse_label(18, 8, 354 - cw, 0.5f, COL_WHITE, g_search_query[0] ? g_search_query : "Search all libraries");
    C2D_DrawRectSolid(0, 35, 0, 400, 1, 0xFF404040);
    if (search_running() && g_search_job_count) {
        float done = (float)g_search_jobs_done / g_search_job_count;
        float pulse = (osGetTime() % 900) / 900.0f;
        C2D_DrawRectSolid(0, 34, 0, 400 * done, 2, COL_PRIMARY);
        C2D_DrawRectSolid(400 * done + (400 - 400 * done) * pulse, 34, 0, 24, 2, COL_PRIMARY_DARK);
    }
}

static void search_draw_top(void)
{
    C2D_DrawRectSolid(0, 0, 0, 400, 240, COL_BG);
    float scroll = g_browse_scroll.value;
    for (int g = 0; g < SG_COUNT; ++g) {
        int count = g_search_group_count[g];
        if (!count) continue;
        float y = BROWSE_CONTENT_TOP + search_group_y(g) - scroll;
        if (y >= BROWSE_CONTENT_BOTTOM || y + search_group_height(g) < BROWSE_CONTENT_TOP) continue;
        bool row_focus = g == g_search_focus;
        if (y >= BROWSE_CONTENT_TOP && y + 18 < BROWSE_CONTENT_BOTTOM) {
            if (row_focus) C2D_DrawRectSolid(10, y + 3, 0, 3, 12, COL_PRIMARY);
            draw_text(row_focus ? 18 : 10, y, 0.55f, row_focus ? COL_WHITE : COL_MUTED, "%s", SEARCH_GROUP_NAMES[g]);
            char counter[32];
            if (row_focus) snprintf(counter, sizeof(counter), "%d / %d", g_search_group_selected[g] + 1, count);
            else snprintf(counter, sizeof(counter), "%d", count);
            float cw = text_width_for_scale(counter, 0.45f);
            draw_text(390 - cw, y + 2, 0.45f, COL_MUTED, "%s", counter);
            int first = g_search_group_first[g], cols = search_group_columns(g);
            if (first > 0) draw_text(390 - cw - 22, y + 1, 0.5f, COL_PRIMARY, "<");
            if (first + cols < count) draw_text(390 - cw - 12, y + 1, 0.5f, COL_PRIMARY, ">");
        }
        int first = g_search_group_first[g], cols = search_group_columns(g);
        float step = search_group_step(g);
        for (int i = first; i < count && i < first + cols; ++i)
            search_draw_card(g_search_groups[g][i], g, 10 + (i - first) * step, y + SEARCH_HEADER_H,
                             row_focus && i == g_search_group_selected[g]);
    }

    if (!g_search_count) {
        bool busy = search_running();
        draw_text_centered(200, 108, 0.6f, COL_WHITE, "%s",
                           busy ? "Searching all libraries" : g_search_jobs_failed ? "Search failed" : "No results");
        char line[160];
        if (busy) snprintf(line, sizeof(line), "%d of %d searches done", g_search_jobs_done, g_search_job_count);
        else if (g_search_jobs_failed) copy_safe(line, sizeof(line), "Check your connection. SELECT: retry   X: New search");
        else snprintf(line, sizeof(line), "Nothing matched \"%s\". Press X to try again.", g_search_query);
        search_label_centered(200, 134, 370, 0.5f, COL_MUTED, line);
    }

    /* Scrollbar */
    float visible = BROWSE_CONTENT_BOTTOM - BROWSE_CONTENT_TOP, total = search_total_height();
    if (total > visible) {
        float thumb = visible * visible / total;
        if (thumb < 16) thumb = 16;
        float pos = scroll / (total - visible);
        if (pos < 0) pos = 0;
        if (pos > 1) pos = 1;
        C2D_DrawRectSolid(396, BROWSE_CONTENT_TOP, 0, 3, visible, 0xFF383030);
        C2D_DrawRectSolid(396, BROWSE_CONTENT_TOP + pos * (visible - thumb), 0, 3, thumb, COL_PRIMARY);
    }

    search_draw_bar();
    C2D_DrawRectSolid(0, 220, 0, 400, 20, COL_BG);
    C2D_DrawRectSolid(0, 220, 0, 400, 1, 0xFF383030);
    browse_label(10, 222, 380, 0.5f, COL_PRIMARY, g_search_focus < 0 ?
                 "A: Edit search   Down: Results   B: Back" :
                 "A: Open   X: New search   B: Back   L/R: Libraries");
}

static void search_draw_bottom(void)
{
    MediaItem *item = search_focused_item();
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_BG);
    if (!item) {
        search_draw_icon(160, 48, 44, COL_PRIMARY);
        draw_text_centered(160, 86, 0.65f, COL_WHITE, "Search all libraries");
        draw_text_lines(24, 116, 0.5f, 272, COL_MUTED,
                        "Movies, shows, episodes, albums, artists and songs from every library.", 3, 18);
        C2D_DrawRectSolid(10, BROWSE_ACTION_TOP, 0, 300, 27, COL_PRIMARY_DARK);
        draw_text_centered(160, BROWSE_ACTION_TOP + 5, 0.5f, COL_WHITE, "%s",
                           g_search_query[0] ? "Edit search  (X)" : "Type a search  (X)");
        return;
    }

    int group = search_group_for(item);
    BrowseArtKind art = search_group_art(group);
    float aw = art == BROWSE_ART_EPISODE ? 136 : art == BROWSE_ART_SQUARE ? 100 : 84;
    float ah = art == BROWSE_ART_EPISODE ? 77 : art == BROWSE_ART_SQUARE ? 100 : 120;
    browse_art(item, art, 10, 10, aw, ah, false);
    if (item->played) search_draw_check(10 + aw - 9, 19);

    float tx = 10 + aw + 10, tw = 310 - tx;
    float px = tx;
    px += search_pill(px, 10, tw, 0.42f, COL_PRIMARY_DARK, COL_WHITE, search_kind_label(item)) + 5;
    int lib = search_focused_library();
    if (lib >= 0 && lib < g_library_count && px < 300)
        search_pill(px, 10, 310 - px, 0.42f, 0xFF3A3434, COL_WHITE, g_libraries[lib].name);

    bool episode = !strcmp(item->type, "Episode");
    const char *title = episode && item->series_name[0] ? item->series_name : item->name;
    draw_text_lines(tx, 30, 0.58f, tw, COL_WHITE, title, 2, 19);

    char sub[256] = "", meta[192] = "";
    browse_format_meta(item, meta, sizeof(meta));
    if (episode) snprintf(sub, sizeof(sub), "%s - %s", meta, item->name);
    else if (music_is_audio(item)) {
        char duration[32] = "";
        if (item->runtime_ticks) format_playback_time(item->runtime_ticks, duration, sizeof(duration));
        snprintf(sub, sizeof(sub), "%s", item->artist[0] ? item->artist : "Unknown artist");
        snprintf(meta, sizeof(meta), "%s%s%s", item->album[0] ? item->album : "", item->album[0] && duration[0] ? "  /  " : "", duration);
    } else if (!strcmp(item->type, "MusicAlbum")) {
        copy_safe(sub, sizeof(sub), item->artist[0] ? item->artist : "Various artists");
        if (item->year > 0) snprintf(meta, sizeof(meta), "%d", item->year); else meta[0] = 0;
    } else if (!strcmp(item->type, "Series")) {
        if (item->year > 0) snprintf(sub, sizeof(sub), "%d", item->year);
        if (item->child_count > 0) {
            char seasons[32];
            snprintf(seasons, sizeof(seasons), "%s%d season%s", sub[0] ? "  /  " : "", item->child_count, item->child_count == 1 ? "" : "s");
            strncat(sub, seasons, sizeof(sub) - strlen(sub) - 1);
        }
        meta[0] = 0;
    } else {
        copy_safe(sub, sizeof(sub), meta);
        meta[0] = 0;
    }
    float y = 70;
    if (sub[0]) { draw_text_lines(tx, y, 0.5f, tw, COL_MUTED, sub, 2, 16); y += sub[0] && text_width_for_scale(sub, 0.5f) > tw ? 34 : 18; }
    if (meta[0]) { browse_label(tx, y, tw, 0.5f, COL_MUTED, meta); y += 18; }

    float oy = 10 + ah + 8;
    if (oy < y + 4) oy = y + 4;
    u64 resume = media_resume_position(item);
    int lines = (int)((BROWSE_ACTION_TOP - 4 - oy - (resume ? 18 : 0)) / 16);
    if (item->overview[0] && lines > 0) draw_text_lines(10, oy, 0.48f, 300, COL_MUTED, item->overview, lines, 16);
    if (resume && !music_is_audio(item)) {
        char at[32];
        format_playback_time(resume, at, sizeof(at));
        draw_text(10, BROWSE_ACTION_TOP - 20, 0.5f, COL_PRIMARY, "Resume at %s", at);
    }

    C2D_DrawRectSolid(10, BROWSE_ACTION_TOP, 0, 145, 27, COL_PRIMARY_DARK);
    draw_text_centered(82, BROWSE_ACTION_TOP + 5, 0.5f, COL_WHITE, "%s",
                       is_playable(item) ? (resume && !music_is_audio(item) ? "Resume" : "Play") : "Open");
    C2D_DrawRectSolid(165, BROWSE_ACTION_TOP, 0, 145, 27, 0xFF30232D);
    draw_text_centered(237, BROWSE_ACTION_TOP + 5, 0.5f, item->is_favorite ? COL_SECONDARY : COL_WHITE,
                       "%s", item->is_favorite ? "Unfavorite" : "Favorite");
}

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <curl/curl.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int Result;
#define HOME_ROW_ITEMS 24
#define MAX_LIBRARIES 64
#define MAX_ITEMS 2048
#include "browse_constants.inc"
#define KEY_UP 1
#define KEY_DOWN 2
#define KEY_LEFT 4
#define KEY_RIGHT 8
#define APP_VERSION "test"
#define TICKS_PER_SECOND 10000000ULL
#define R_SUCCEEDED(x) ((x) >= 0)
#include "browse_types.inc"
static struct { char server[256], token[192], user_id[80], device_id[80]; } g_cfg;
static MediaItem g_current, g_items[MAX_ITEMS], g_libraries[MAX_LIBRARIES];
static HomeRow g_home_rows[MAX_LIBRARIES + 2];
static int g_library_count, g_item_count, g_home_row, g_home_scroll, g_library_selected, g_library_first;
static int g_selected, g_scroll, g_nav_tab, g_active_tab;
static bool g_tab_focus, g_home_dirty;
static View g_view;
static BrowseScroll g_browse_scroll;
static View g_browse_scroll_view;
static char g_current_parent_id[80], g_current_parent_type[40], g_search_query[96], g_screen_title[96];
static char g_browse_scroll_parent[80], g_browse_scroll_search[96];
static int g_browse_repeat_key;
static u64 g_browse_repeat_at, test_time;
static u64 osGetTime(void) { return test_time; }
static char g_play_media_source_id[128] = "source", g_play_session[96] = "session";
static void json_escape(const char *, char *, size_t);
static int playback_report_volume_level(int, bool);
static u64 clamp_media_ticks(u64 ticks) { return ticks; }
#include "browse_support.inc"
static void browse_cancel_request(void) {}
static void set_status(const char *fmt, ...) { (void)fmt; }
static void set_http_failure(const char *s, const HttpResponse *r, Result ret) { (void)s; (void)r; (void)ret; }

static size_t receive(void *data, size_t size, size_t count, void *arg)
{
    HttpResponse *r = arg;
    size_t bytes = size * count;
    r->body = realloc(r->body, r->size + bytes + 1);
    assert(r->body);
    memcpy(r->body + r->size, data, bytes);
    r->size += bytes;
    r->body[r->size] = 0;
    return bytes;
}
static Result request(const char *path, const char *method, const char *body, HttpResponse *res)
{
    memset(res, 0, sizeof(*res));
    CURL *curl = curl_easy_init();
    char url[1024], auth[384], header[448];
    build_url(url, sizeof(url), path);
    auth_header(auth, sizeof(auth), true);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    struct curl_slist *headers = curl_slist_append(NULL, header);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, res);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    res->status = status;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result == CURLE_OK ? 0 : -1;
}
static Result api_get(const char *path, HttpResponse *r) { return request(path, "GET", NULL, r); }
static Result api_post(const char *path, const char *body, bool token, HttpResponse *r) { (void)token; return request(path, "POST", body, r); }
static Result api_delete(const char *path, HttpResponse *r) { return request(path, "DELETE", NULL, r); }
#include "../source/parts/home_api.inc"
#include "browse_navigation.inc"
static bool g_setup_resume_session, g_setup_started;
static int g_setup_row, setup_scans;
static void setup_cancel_http(void) {}
static void setup_start_scan(void) { ++setup_scans; g_view = VIEW_SETUP; }
/* Fixed glyph advances isolate truncation/UTF-8 behavior from device fonts. */
static float text_width_for_scale(const char *text, float scale)
{
    float width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xC0) != 0x80) width += 20 * scale;
    return width;
}
typedef struct { float x, y, scale; char text[256]; } DrawnText;
static DrawnText drawn_text[64];
static int drawn_count, drawn_art_count;
static BrowseArtKind drawn_art_kind;
static char drawn_art_id[80];
static const u32 COL_WHITE = 0xFFFFFFFF, COL_MUTED = 0xFFB5B5B5;
static const u32 COL_PRIMARY = 0xFFDCA400, COL_SECONDARY = 0xFFC35CAA;
static void draw_text(float x, float y, float scale, u32 color, const char *format, ...)
{
    (void)color;
    assert(drawn_count < 64);
    DrawnText *draw = &drawn_text[drawn_count++];
    draw->x = x; draw->y = y; draw->scale = scale;
    va_list args; va_start(args, format);
    vsnprintf(draw->text, sizeof(draw->text), format, args);
    va_end(args);
}
static void C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 color)
{
    (void)x; (void)y; (void)z; (void)color;
    assert(w >= 0 && h >= 0);
}
static void browse_art(const MediaItem *item, BrowseArtKind kind, float x, float y, float w, float h, bool clip)
{
    (void)x; (void)y; (void)w; (void)h; (void)clip;
    ++drawn_art_count; drawn_art_kind = kind;
    copy_safe(drawn_art_id, sizeof(drawn_art_id), browse_image_id(item, kind));
}
static const char *display_item_kind(const MediaItem *item) { return item->type; }
#include "browse_typography.inc"
#include "browse_episodes.inc"

static void check_text_bounds(float width, float top, float bottom)
{
    for (int i = 0; i < drawn_count; ++i) {
        DrawnText *draw = &drawn_text[i];
        assert(draw->scale >= 0.5f && draw->x >= 0);
        assert(draw->x + text_width_for_scale(draw->text, draw->scale) <= width);
        assert(draw->y >= top && draw->y + 30 * draw->scale <= bottom);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    copy_safe(g_cfg.server, sizeof(g_cfg.server), argv[1]);
    strcpy(g_cfg.user_id, "user1"); strcpy(g_cfg.token, "test-token");
    strcpy(g_cfg.device_id, "test-device");
    g_library_count = 6;
    strcpy(g_libraries[0].id, "library1");
    browse_home_reset();
    for (int row = 1; row <= 3; ++row) {
        char path[768]; HttpResponse r;
        build_home_row_path(row, path, sizeof(path));
        assert(api_get(path, &r) == 0 && r.status == 200);
        int count = 0;
        assert(parse_items(r.body, g_items, &count, MAX_ITEMS, true, NULL));
        assert(count == (row == 2 ? 0 : 1));
        free_response(&r);
    }
    g_item_count = 1;
    g_current = g_items[0];
    assert(!strcmp(g_current.series_name, "A Show") && g_current.series_thumb);
    assert(g_current.parent_index_number == 1 && g_current.index_number == 2);
    assert(media_resume_position(&g_current) == 9876543210ULL);
    g_current.played = true; assert(!media_resume_position(&g_current));
    g_current.played = false; g_current.position_ticks = g_current.runtime_ticks;
    assert(!media_resume_position(&g_current));
    refresh_play_item(); assert(media_resume_position(&g_current) == 9876543210ULL);
    int count = 0;
    assert(!parse_items("{\"Items\":[", g_items, &count, 10, true, NULL));
    assert(!parse_items("[{\"Id\":\"bad\"}", g_items, &count, 10, true, NULL));
    assert(parse_items("[]", g_items, &count, 10, true, NULL) && count == 0);
    const char *nested = "[{\"Id\":\"outer\",\"Name\":\"Name\",\"IsFavorite\":false,\"UserData\":{\"IsFavorite\":true,\"PlaybackPositionTicks\":9000000001}}]";
    assert(parse_items(nested, g_items, &count, 10, true, NULL));
    assert(g_items[0].is_favorite && g_items[0].position_ticks == 9000000001ULL);
    g_items[0] = g_current;
    g_home_rows[0].count = 1; g_home_rows[0].items[0] = g_current;
    assert(toggle_favorite(&g_items[0]));
    assert(g_items[0].is_favorite && g_home_rows[0].items[0].is_favorite);
    char path[768], encoded[100]; HttpResponse r;
    url_encode(FAVORITES_PARENT_ID, encoded, sizeof(encoded));
    build_items_page_path(path, sizeof(path), encoded, 100, 100, false);
    assert(api_get(path, &r) == 0 && r.status == 200); free_response(&r);
    assert(toggle_favorite(&g_items[0]) && !g_items[0].is_favorite);
    MediaItem failed = g_current; strcpy(failed.id, "fail");
    assert(!toggle_favorite(&failed) && !failed.is_favorite);
    char body[1024];
    build_playback_state_body(body, sizeof(body), 9876543210ULL, true, false, 80);
    assert(send_playback_report("/Sessions/Playing", body));
    assert(send_playback_report("/Sessions/Playing/Progress", body));
    assert(!send_playback_report("/Sessions/Playing/Rejected", body));
    build_playback_stop_body(body, sizeof(body), 9876543210ULL);
    assert(send_playback_report("/Sessions/Playing/Stopped", body));
    g_view = VIEW_LIBRARIES; g_home_row = 0;
    /* Home visibly continues before the user has ever pressed Down. */
    assert(BROWSE_CONTENT_TOP + home_y(1) + 18 < BROWSE_CONTENT_BOTTOM);
    assert(BROWSE_CONTENT_TOP + home_y(1) + HOME_CARD_TOP <= BROWSE_CONTENT_BOTTOM - 8);
    assert(GRID_CARD_TOP + GRID_ROW_HEIGHT <= BROWSE_CONTENT_BOTTOM - 8);
    for (int i = 0; i < 8; ++i) browse_move(KEY_RIGHT);
    assert(g_library_selected == 5 && g_library_first == 4);
    assert(browse_focused() == &g_libraries[5]);
    browse_move(KEY_UP); assert(g_tab_focus);
    for (int i = 0; i < 20; ++i) browse_move(KEY_RIGHT);
    assert(g_nav_tab == 7 && browse_focused() == &g_libraries[5]);
    browse_move(KEY_DOWN); assert(!g_tab_focus);
    for (int i = 0; i < 20; ++i) browse_move(KEY_DOWN);
    assert(g_home_row == 8 && g_home_scroll > 0);
    g_home_rows[7].loaded = true; g_home_rows[7].count = 0;
    browse_clamp(); assert(!browse_focused());
    assert(g_home_scroll + BROWSE_CONTENT_BOTTOM - BROWSE_CONTENT_TOP == home_y(9));
    browse_move(KEY_UP);
    assert(g_home_scroll == home_y(7));
    g_view = VIEW_ITEMS; g_item_count = 12; g_selected = 0;
    browse_move(KEY_DOWN); assert(g_selected == 4 && g_scroll == 4);
    browse_move(KEY_DOWN); assert(g_selected == 8 && g_scroll == 8);
    for (int i = 0; i < 8; ++i) browse_move(KEY_RIGHT);
    assert(g_selected == 11);
    g_item_count = 0; browse_clamp(); assert(!browse_focused());
    strcpy(g_current_parent_type, "Season");
    g_item_count = 6; g_selected = g_scroll = 0;
    assert(browse_is_episode_list());
    browse_move(KEY_DOWN); assert(g_selected == 1 && g_scroll == 0);
    browse_move(KEY_DOWN); assert(g_selected == 2 && g_scroll == 1);
    browse_move(KEY_DOWN); assert(g_selected == 3 && g_scroll == 2);
    browse_move(KEY_UP); assert(g_selected == 2 && g_scroll == 2);
    browse_move(KEY_UP); assert(g_selected == 1 && g_scroll == 1);
    browse_move(KEY_LEFT); assert(g_selected == 0 && g_scroll == 0);
    browse_move(KEY_UP); assert(g_tab_focus);
    browse_move(KEY_DOWN); assert(!g_tab_focus && g_selected == 0);
    for (int i = 0; i < 20; ++i) browse_move(KEY_DOWN);
    assert(g_selected == 5 && g_scroll == 4 && browse_focused() == &g_items[5]);
    browse_update_scroll(); assert(g_browse_scroll.target == 4 * EPISODE_ROW_HEIGHT);
    g_item_count = 1; browse_clamp(); assert(g_selected == 0 && g_scroll == 0);
    g_item_count = 0; browse_clamp(); assert(!browse_focused() && !g_scroll);
    g_view = VIEW_SEARCH; assert(!browse_is_episode_list());
    g_view = VIEW_ITEMS; strcpy(g_current_parent_type, "Series");
    g_item_count = 12; g_selected = 0;
    browse_move(KEY_DOWN); assert(g_selected == 4 && g_scroll == 4);
    g_current_parent_type[0] = 0;
    /* Episode stills must not alias the shared series artwork in the cache. */
    assert(!strcmp(browse_image_id(&g_current, BROWSE_ART_EPISODE), "ep1"));
    assert(!strcmp(browse_image_id(&g_current, BROWSE_ART_LANDSCAPE), "show1"));
    assert(!strcmp(browse_image_id(&g_current, BROWSE_ART_POSTER), "show1"));
    browse_build_image_path(&g_current, BROWSE_ART_EPISODE, path, sizeof(path));
    assert(strstr(path, "/Items/ep1/Images/Primary?") && strstr(path, "fillWidth=184&fillHeight=104"));
    browse_build_image_path(&g_current, BROWSE_ART_LANDSCAPE, path, sizeof(path));
    assert(strstr(path, "/Items/show1/Images/Thumb?"));
    MediaItem ep = g_current; strcpy(ep.id, "ep2");
    assert(!strcmp(browse_image_id(&ep, BROWSE_ART_EPISODE), "ep2"));
    ep.series_thumb = false;
    assert(!strcmp(browse_image_id(&ep, BROWSE_ART_LANDSCAPE), "ep2"));
    char episode_meta[128];
    browse_episode_meta(&ep, episode_meta, sizeof(episode_meta));
    assert(strstr(episode_meta, "S1:E2") && strstr(episode_meta, "30 min") && strstr(episode_meta, "In progress"));
    ep.played = true;
    browse_episode_meta(&ep, episode_meta, sizeof(episode_meta));
    assert(strstr(episode_meta, "Watched") && !strstr(episode_meta, "In progress"));
    ep.index_number = ep.parent_index_number = -1; ep.runtime_ticks = 0;
    browse_episode_meta(&ep, episode_meta, sizeof(episode_meta));
    assert(!strstr(episode_meta, "-1") && !strstr(episode_meta, "0 min"));
    ep = g_current; ep.is_favorite = true;
    strcpy(ep.name, "A long episode title that needs two readable lines instead of a tiny poster caption");
    strcpy(ep.overview, "The group set out on a new adventure, but a surprise encounter changes their plans.");
    g_view = VIEW_ITEMS; strcpy(g_current_parent_type, "Season");
    for (int offset = 0; offset <= EPISODE_ROW_HEIGHT; offset += 7) {
        drawn_count = drawn_art_count = 0;
        for (int row = 0; row < 3; ++row)
            draw_episode_row(&ep, GRID_CARD_TOP + row * EPISODE_ROW_HEIGHT - offset, row == 1);
        assert(drawn_count > 0 && drawn_art_count == 3 && drawn_art_kind == BROWSE_ART_EPISODE);
        assert(!strcmp(drawn_art_id, "ep1"));
        check_text_bounds(400, GRID_CARD_TOP - 2, BROWSE_CONTENT_BOTTOM);
    }
    drawn_count = drawn_art_count = 0;
    draw_episode_preview(&ep);
    assert(drawn_art_count == 1 && drawn_art_kind == BROWSE_ART_EPISODE);
    check_text_bounds(320, 0, BROWSE_ACTION_TOP);
    ep.overview[0] = 0; ep.series_name[0] = 0; ep.position_ticks = 0;
    strcpy(g_screen_title, "Season 2");
    drawn_count = 0; draw_episode_preview(&ep);
    check_text_bounds(320, 0, BROWSE_ACTION_TOP);
    assert(!strcmp(drawn_text[0].text, "Season 2"));
    g_current_parent_type[0] = 0;
    g_browse_scroll.ready = false;
    BrowseScroll motion = {0};
    browse_scroll_step(&motion, 0, 1000);
    browse_scroll_step(&motion, 150, 1010);
    assert(motion.value == 0); /* Selection changes never teleport the viewport. */
    float previous = motion.value;
    for (int ms = 17; ms < BROWSE_SCROLL_MS; ms += 17) {
        browse_scroll_step(&motion, 150, 1010 + ms);
        assert(motion.value > previous && motion.value < 150);
        previous = motion.value;
    }
    browse_scroll_step(&motion, 150, 1010 + BROWSE_SCROLL_MS);
    assert(motion.value == 150);
    browse_scroll_step(&motion, 300, 1300);
    browse_scroll_step(&motion, 300, 1360);
    previous = motion.value;
    browse_scroll_step(&motion, 0, 1360); /* Reverse halfway through a movement. */
    assert(motion.value == previous);
    browse_scroll_step(&motion, 0, 1387);
    assert(motion.value > 0 && motion.value < previous);
    browse_scroll_step(&motion, 0, 1600); /* A slow frame still lands exactly. */
    assert(motion.value == 0);
    g_view = VIEW_LIBRARIES; g_home_row = 0; browse_clamp();
    test_time = 2000; browse_update_scroll();
    browse_move(KEY_DOWN); browse_update_scroll();
    assert(g_browse_scroll.value == 0 && g_browse_scroll.target > 0);
    test_time += 60; browse_update_scroll();
    assert(g_browse_scroll.value > 0 && g_browse_scroll.value < g_browse_scroll.target);
    g_view = VIEW_ITEMS; g_scroll = 0; browse_update_scroll();
    assert(g_browse_scroll.value == 0); /* Opening another screen must not glide from Home. */
    g_scroll = 8; strcpy(g_current_parent_id, "other-library"); browse_update_scroll();
    assert(g_browse_scroll.value == 2 * GRID_ROW_HEIGHT);
    test_time = 3000;
    assert(browse_repeat(KEY_DOWN, KEY_DOWN) == KEY_DOWN);
    test_time = 3250; assert(!browse_repeat(0, KEY_DOWN));
    test_time = 3290; assert(browse_repeat(0, KEY_DOWN) == KEY_DOWN);
    test_time = 3400; assert(!browse_repeat(0, KEY_DOWN));
    test_time = 3490; assert(browse_repeat(0, KEY_DOWN) == KEY_DOWN);
    assert(browse_repeat(KEY_UP, KEY_UP) == KEY_UP);
    assert(!browse_repeat(0, 0) && !g_browse_repeat_key);
    char label[100] = "Anime";
    trim_text_to_width(label, sizeof(label), 0.5f, 50, true);
    assert(!strcmp(label, "Anime")); /* Exact fit never gets a spurious suffix. */
    strcpy(label, "Anime Subs");
    trim_text_to_width(label, sizeof(label), 0.5f, 120, true);
    assert(!strcmp(label, "Anime Subs"));
    strcpy(label, "A very long library name");
    trim_text_to_width(label, sizeof(label), 0.5f, 80, true);
    assert(strstr(label, "...") && text_width_for_scale(label, 0.5f) <= 80);
    strcpy(label, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    text_remove_last_character(label);
    assert(!strcmp(label, "\xE6\x97\xA5\xE6\x9C\xAC"));
    strcpy(label, "Short");
    fit_text_to_width(label, sizeof(label), 0.5f, 120, true, true);
    assert(!strcmp(label, "Short...")); /* Hidden wrapped lines still indicate more. */
    g_setup_resume_session = true; g_tab_focus = true; g_browse_repeat_key = KEY_UP;
    open_server_setup();
    assert(g_view == VIEW_SETUP && setup_scans == 1 && g_setup_started && !g_setup_resume_session);
    assert(!g_tab_focus && !g_browse_repeat_key && !strcmp(g_cfg.token, "test-token"));
    puts("PASS: production parsing, user state, >32-bit resume, completion, favorite failure, empty/scrolled navigation");
    puts("PASS: fitting labels, exact widths, UTF-8 truncation, explicit server setup without clearing login");
    puts("PASS: next-row visibility, scroll boundaries, animation continuity/reversal, screen changes, held navigation");
    puts("PASS: episode list navigation, distinct episode thumbnails, watch state, readable titles and clipped layout");
    curl_global_cleanup();
}

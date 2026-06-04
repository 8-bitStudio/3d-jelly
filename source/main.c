#include <3ds.h>
#include <citro2d.h>
#include "picojpeg.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_VERSION "0.1.2"
#define CONFIG_DIR "sdmc:/3dJelly"
#define CONFIG_PATH "sdmc:/3dJelly/config.ini"

#define MAX_ITEMS 36
#define MAX_STACK 8
#define HTTP_CAP (1024 * 1024)
#define HTTP_STATUS_NONE 0xFFFFFFFFu
#define HTTP_STATUS_TIMEOUT_NS 60000000000ULL
#define STREAM_URL_CAP 2048
#define STREAM_READ_SIZE (188 * 16)
#define H264_BUFFER_CAP (1024 * 1024)
#define MVD_IN_CAP (1024 * 1024)
#define MVD_OUT_CAP (1024 * 1024)
#define MJPEG_READ_SIZE 4096
#define MJPEG_FRAME_CAP (1024 * 1024)
#define JPEG_PIXELS_CAP (854 * 480)
#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_CHANNELS 1
#define AUDIO_WAVEBUF_COUNT 4
#define AUDIO_WAVEBUF_SAMPLES 1024
#define AUDIO_READ_SIZE 4096
#define AUDIO_PCM_BUFFER_BYTES (AUDIO_WAVEBUF_SAMPLES * AUDIO_CHANNELS * sizeof(s16))

typedef enum {
    VIEW_SETUP,
    VIEW_LIBRARIES,
    VIEW_ITEMS,
    VIEW_DETAIL,
    VIEW_PLAYBACK,
    VIEW_TESTS
} View;

typedef struct {
    char id[80];
    char name[128];
    char type[40];
    char collection_type[40];
    bool is_folder;
    int year;
    unsigned long long runtime_ticks;
} MediaItem;

typedef struct {
    char parent_id[80];
    char title[96];
    int selected;
    int scroll;
} NavFrame;

typedef struct {
    char server[256];
    char username[96];
    char password[96];
    char token[192];
    char user_id[80];
    char device_id[80];
    int quality; /* 144, 240, 360, or 480 */
} Config;

typedef struct {
    Result result;
    u32 status;
    char *body;
    size_t size;
    char url[768];
} HttpResponse;

typedef struct {
    int width;
    int height;
    int video_bitrate;
    int audio_bitrate;
    int max_fps;
} QualityProfile;

static Config g_cfg;
static View g_view = VIEW_SETUP;
static C3D_RenderTarget *g_top;
static C3D_RenderTarget *g_bottom;
static C2D_TextBuf g_text;
static bool g_ui_ready;

static MediaItem g_libraries[MAX_ITEMS];
static int g_library_count;
static MediaItem g_items[MAX_ITEMS];
static int g_item_count;
static MediaItem g_current;
static NavFrame g_stack[MAX_STACK];
static int g_stack_depth;
static int g_selected;
static int g_scroll;
static int g_setup_row;
static char g_screen_title[96] = "Libraries";
static char g_current_parent_id[80];
static char g_status[192] = "Press Y to configure a Jellyfin server.";
static char g_play_url[STREAM_URL_CAP];
static char g_play_method[64];
static char g_play_session[96];
static char g_play_media_source_id[128];
static char g_play_status[192];
static View g_return_view = VIEW_ITEMS;
static unsigned g_frame_counter;
static bool g_exit_requested;
static bool g_is_new_3ds;

static const u32 COL_BG = 0xFF101010;
static const u32 COL_PAPER = 0xFF202020;
static const u32 COL_CARD = 0xFF00455C;
static const u32 COL_CARD_2 = 0xFF1C4C5C;
static const u32 COL_PRIMARY = 0xFF00A4DC;
static const u32 COL_PRIMARY_DARK = 0xFF00729A;
static const u32 COL_SECONDARY = 0xFFAA5CC3;
static const u32 COL_WHITE = 0xFFFFFFFF;
static const u32 COL_MUTED = 0xFFB5B5B5;

static void ui_graphics_init(void);
static void ui_graphics_exit(void);
static void save_config(void);
static void build_url(char *out, size_t outsz, const char *path);
static bool play_stream_url(const char *url);
static bool play_mjpeg_stream_url(const char *url, bool avi_container);
static bool play_current_item_video(void);

static const int QUALITY_LEVELS[] = {144, 240, 360, 480};

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

static int quality_count(void)
{
    return (int)(sizeof(QUALITY_LEVELS) / sizeof(QUALITY_LEVELS[0]));
}

static int quality_index(int quality)
{
    for (int i = 0; i < quality_count(); i++) {
        if (QUALITY_LEVELS[i] == quality) {
            return i;
        }
    }
    return -1;
}

static bool is_supported_quality(int quality)
{
    return quality_index(quality) >= 0;
}

static int default_quality(void)
{
    return g_is_new_3ds ? 240 : 144;
}

static QualityProfile quality_profile(void)
{
    switch (g_cfg.quality) {
    case 144: {
        QualityProfile q = {256, 144, 280000, 48000, 24};
        return q;
    }
    case 360: {
        QualityProfile q = {640, 360, 700000, 96000, 24};
        return q;
    }
    case 480: {
        QualityProfile q = {854, 480, 1200000, 128000, 24};
        return q;
    }
    case 240:
    default: {
        QualityProfile q = {400, 240, 360000, 64000, 24};
        return q;
    }
    }
}

static int mjpeg_target_fps(void)
{
    if (g_is_new_3ds) {
        switch (g_cfg.quality) {
        case 144:
            return 15;
        case 360:
            return 10;
        case 480:
            return 8;
        case 240:
        default:
            return 12;
        }
    }
    switch (g_cfg.quality) {
    case 360:
        return 8;
    case 480:
        return 6;
    case 144:
    case 240:
    default:
        return 12;
    }
}

static int mjpeg_target_bitrate(void)
{
    if (g_is_new_3ds) {
        switch (g_cfg.quality) {
        case 144:
            return 520000;
        case 360:
            return 1100000;
        case 480:
            return 1600000;
        case 240:
        default:
            return 760000;
        }
    }
    switch (g_cfg.quality) {
    case 144:
        return 320000;
    case 360:
        return 850000;
    case 480:
        return 1200000;
    case 240:
    default:
        return 560000;
    }
}

static void detect_hardware(void)
{
    bool is_new = false;
    g_is_new_3ds = R_SUCCEEDED(APT_CheckNew3DS(&is_new)) && is_new;
}

static void apply_hardware_defaults(void)
{
    if (!is_supported_quality(g_cfg.quality)) {
        g_cfg.quality = default_quality();
        save_config();
    }
}

static void copy_safe(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void trim_newline(char *s)
{
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = 0;
    }
}

static void trim_edges(char *s)
{
    if (!s) {
        return;
    }
    trim_newline(s);
    while (*s == ' ' || *s == '\t') {
        memmove(s, s + 1, strlen(s));
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static void strip_trailing_slash(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') {
        s[--n] = 0;
    }
}

static bool starts_with_http(const char *s);

static void normalize_server_url(char *s)
{
    if (!s || !s[0]) {
        return;
    }

    trim_edges(s);

    if (!starts_with_http(s)) {
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "http://%s", s);
        copy_safe(s, 256, tmp);
    }

    char *q = strchr(s, '?');
    if (q) {
        *q = 0;
    }
    char *hash = strchr(s, '#');
    if (hash) {
        *hash = 0;
    }

    strip_trailing_slash(s);

    const char *suffixes[] = {
        "/web/index.html",
        "/web",
        "/login.html"
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t n = strlen(s);
        size_t m = strlen(suffixes[i]);
        if (n >= m && strcmp(s + n - m, suffixes[i]) == 0) {
            s[n - m] = 0;
            strip_trailing_slash(s);
            break;
        }
    }
}

static bool starts_with_http(const char *s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

static void ensure_defaults(void)
{
    if (!g_cfg.server[0]) {
        copy_safe(g_cfg.server, sizeof(g_cfg.server), "http://192.168.1.2:8096");
    }
    normalize_server_url(g_cfg.server);

    if (!g_cfg.device_id[0]) {
        snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "3dJelly-%08lX", (unsigned long)osGetTime());
    }
    if (!is_supported_quality(g_cfg.quality)) {
        g_cfg.quality = default_quality();
    }
}

static void save_config(void)
{
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return;
    }

    fprintf(f, "server=%s\n", g_cfg.server);
    fprintf(f, "username=%s\n", g_cfg.username);
    fprintf(f, "password=%s\n", g_cfg.password);
    fprintf(f, "token=%s\n", g_cfg.token);
    fprintf(f, "user_id=%s\n", g_cfg.user_id);
    fprintf(f, "device_id=%s\n", g_cfg.device_id);
    fprintf(f, "quality=%d\n", g_cfg.quality);
    fclose(f);
}

static void load_config(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ensure_defaults();
        return;
    }

    char line[384];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq++ = 0;
        if (strcmp(line, "server") == 0) {
            copy_safe(g_cfg.server, sizeof(g_cfg.server), eq);
        } else if (strcmp(line, "username") == 0) {
            copy_safe(g_cfg.username, sizeof(g_cfg.username), eq);
        } else if (strcmp(line, "password") == 0) {
            copy_safe(g_cfg.password, sizeof(g_cfg.password), eq);
        } else if (strcmp(line, "token") == 0) {
            copy_safe(g_cfg.token, sizeof(g_cfg.token), eq);
        } else if (strcmp(line, "user_id") == 0) {
            copy_safe(g_cfg.user_id, sizeof(g_cfg.user_id), eq);
        } else if (strcmp(line, "device_id") == 0) {
            copy_safe(g_cfg.device_id, sizeof(g_cfg.device_id), eq);
        } else if (strcmp(line, "quality") == 0) {
            g_cfg.quality = atoi(eq);
        }
    }
    fclose(f);
    ensure_defaults();
}

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t w = 0;
    if (!outsz) {
        return;
    }
    for (size_t i = 0; in && in[i] && w + 2 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c == '\n') {
            out[w++] = '\\';
            out[w++] = 'n';
        } else if (c == '\r') {
            out[w++] = '\\';
            out[w++] = 'r';
        } else if (c == '\t') {
            out[w++] = '\\';
            out[w++] = 't';
        } else if (c >= 0x20) {
            out[w++] = (char)c;
        }
    }
    out[w] = 0;
}

static void url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    if (!outsz) {
        return;
    }
    for (size_t i = 0; in && in[i] && w + 4 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 15];
        }
    }
    out[w] = 0;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *find_key_range(const char *start, const char *end, const char *key)
{
    size_t klen = strlen(key);
    int depth = 0;
    bool in_str = false;
    bool esc = false;

    for (const char *p = start; p + klen + 2 < end; p++) {
        char c = *p;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }

        if (c == '{' || c == '[') {
            depth++;
            continue;
        }
        if (c == '}' || c == ']') {
            depth--;
            continue;
        }

        if (c != '"') {
            continue;
        }

        if (depth == 1 && (size_t)(end - p) > klen + 2 && strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            const char *q = skip_ws(p + klen + 2, end);
            if (q < end && *q == ':') {
                return skip_ws(q + 1, end);
            }
        }

        in_str = true;
    }
    return NULL;
}

static bool json_get_string_range(const char *start, const char *end, const char *key, char *out, size_t outsz)
{
    const char *p = find_key_range(start, end, key);
    if (!p || p >= end || *p != '"') {
        if (outsz) {
            out[0] = 0;
        }
        return false;
    }

    p++;
    size_t w = 0;
    while (p < end && *p != '"' && w + 1 < outsz) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n':
                out[w++] = '\n';
                break;
            case 'r':
                out[w++] = '\r';
                break;
            case 't':
                out[w++] = '\t';
                break;
            case 'u':
                out[w++] = '?';
                p += (p + 4 < end) ? 4 : 0;
                break;
            default:
                out[w++] = *p;
                break;
            }
        } else {
            out[w++] = *p;
        }
        p++;
    }
    if (outsz) {
        out[w] = 0;
    }
    return true;
}

static bool json_get_bool_range(const char *start, const char *end, const char *key, bool *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    if (p + 4 <= end && strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (p + 5 <= end && strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_get_int_range(const char *start, const char *end, const char *key, int *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    *out = atoi(p);
    return true;
}

static bool json_get_ull_range(const char *start, const char *end, const char *key, unsigned long long *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    *out = strtoull(p, NULL, 10);
    return true;
}

static bool json_object_range_after(const char *p, const char *end, const char **obj_start, const char **obj_end)
{
    p = skip_ws(p, end);
    if (p >= end || *p != '{') {
        return false;
    }

    int depth = 0;
    bool in_str = false;
    bool esc = false;
    const char *q = p;
    for (; q < end; q++) {
        char c = *q;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *obj_start = p;
                *obj_end = q + 1;
                return true;
            }
        }
    }
    return false;
}

static bool json_get_object_range(const char *start, const char *end, const char *key, const char **obj_start, const char **obj_end)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    return json_object_range_after(p, end, obj_start, obj_end);
}

static bool parse_items(const char *json, MediaItem *items, int *count)
{
    const char *end = json + strlen(json);
    const char *p = find_key_range(json, end, "Items");
    if (!p) {
        *count = 0;
        return false;
    }
    p = skip_ws(p, end);
    if (p >= end || *p != '[') {
        *count = 0;
        return false;
    }
    p++;

    int n = 0;
    while (p < end && n < MAX_ITEMS) {
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

        MediaItem item;
        memset(&item, 0, sizeof(item));
        json_get_string_range(os, oe, "Id", item.id, sizeof(item.id));
        json_get_string_range(os, oe, "Name", item.name, sizeof(item.name));
        json_get_string_range(os, oe, "Type", item.type, sizeof(item.type));
        json_get_string_range(os, oe, "CollectionType", item.collection_type, sizeof(item.collection_type));
        json_get_bool_range(os, oe, "IsFolder", &item.is_folder);
        json_get_int_range(os, oe, "ProductionYear", &item.year);
        json_get_ull_range(os, oe, "RunTimeTicks", &item.runtime_ticks);

        if (item.id[0] && item.name[0]) {
            items[n++] = item;
        }
        p = oe;
    }

    *count = n;
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

static Result http_request_full(HTTPC_RequestMethod method, const char *url, const char *body, bool include_token, HttpResponse *out)
{
    memset(out, 0, sizeof(*out));
    out->status = 0;
    out->result = 0;
    copy_safe(out->url, sizeof(out->url), url);

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

    for (int redirects = 0; redirects < 4; redirects++) {
        copy_safe(out->url, sizeof(out->url), active_url);
        ret = httpcOpenContext(&context, method, active_url, method == HTTPC_METHOD_POST ? 0 : 1);
        if (R_FAILED(ret)) {
            break;
        }

        httpcSetSSLOpt(&context, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_ENABLED);
        httpcAddRequestHeaderField(&context, "User-Agent", "3dJelly/0.1 Nintendo 3DS");
        httpcAddRequestHeaderField(&context, "Accept", "application/json, */*");
        httpcAddRequestHeaderField(&context, "Connection", "Keep-Alive");

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
                httpcCloseContext(&context);
                break;
            }
        }

        ret = httpcBeginRequest(&context);
        if (R_FAILED(ret)) {
            httpcCloseContext(&context);
            break;
        }

        ret = httpcGetResponseStatusCodeTimeout(&context, &status, HTTP_STATUS_TIMEOUT_NS);
        if (R_FAILED(ret)) {
            httpcCloseContext(&context);
            break;
        }
        if (status == HTTP_STATUS_NONE) {
            ret = (Result)0xD9000001;
            httpcCloseContext(&context);
            break;
        }

        if ((status >= 301 && status <= 303) || (status >= 307 && status <= 308)) {
            if (!redirect_url) {
                redirect_url = (char *)malloc(1024);
            }
            if (!redirect_url) {
                ret = -1;
                httpcCloseContext(&context);
                break;
            }
            memset(redirect_url, 0, 1024);
            ret = httpcGetResponseHeader(&context, "Location", redirect_url, 1024);
            httpcCloseContext(&context);
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

    httpcCloseContext(&context);
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

static void set_http_failure(const char *prefix, const HttpResponse *res, Result ret)
{
    if (res->status == HTTP_STATUS_NONE) {
        set_status("%s: no HTTP response. Check URL/WiFi: %.72s", prefix, res->url);
    } else if (ret == (Result)HTTPC_RESULTCODE_TIMEDOUT) {
        set_status("%s: timed out waiting for Jellyfin.", prefix);
    } else if (res->status == 401) {
        set_status("%s: HTTP 401. Check username/password.", prefix);
    } else {
        set_status("%s: HTTP %lu result 0x%08lX", prefix, (unsigned long)res->status, (unsigned long)ret);
    }
}

static void format_http_failure(char *out, size_t outsz, const char *prefix, const HttpResponse *res, Result ret)
{
    if (res->status == HTTP_STATUS_NONE) {
        snprintf(out, outsz, "%s: no HTTP response. Check URL/WiFi.", prefix);
    } else if (ret == (Result)HTTPC_RESULTCODE_TIMEDOUT) {
        snprintf(out, outsz, "%s: timed out waiting for Jellyfin.", prefix);
    } else if (res->status == 401) {
        snprintf(out, outsz, "%s: HTTP 401. Check username/password.", prefix);
    } else {
        snprintf(out, outsz, "%s: HTTP %lu result 0x%08lX", prefix, (unsigned long)res->status, (unsigned long)ret);
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
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
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
        set_status("Set server and username first.");
        return false;
    }

    char user[192];
    char pass[192];
    json_escape(g_cfg.username, user, sizeof(user));
    json_escape(g_cfg.password, pass, sizeof(pass));

    char body[512];
    snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}", user, pass);

    HttpResponse res;
    set_status("Logging in...");
    Result ret = api_post("/Users/AuthenticateByName", body, false, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        set_http_failure("Login failed", &res, ret);
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
        set_status("Login response was missing token/user id.");
        return false;
    }

    save_config();
    set_status("Logged in as %s.", g_cfg.username);
    return true;
}

static bool load_libraries(void)
{
    if (!g_cfg.token[0] || !g_cfg.user_id[0]) {
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "/Users/%s/Views?Fields=PrimaryImageAspectRatio", g_cfg.user_id);

    HttpResponse res;
    Result ret = api_get(path, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        set_http_failure("Could not load libraries", &res, ret);
        free_response(&res);
        return false;
    }

    parse_items(res.body, g_libraries, &g_library_count);
    free_response(&res);
    g_selected = 0;
    g_scroll = 0;
    g_stack_depth = 0;
    g_current_parent_id[0] = 0;
    copy_safe(g_screen_title, sizeof(g_screen_title), "Libraries");
    g_view = VIEW_LIBRARIES;
    set_status("Loaded %d libraries.", g_library_count);
    return true;
}

static bool load_items_for_parent(const char *parent_id, const char *title)
{
    char path[360];
    char enc_parent[160];
    url_encode(parent_id, enc_parent, sizeof(enc_parent));
    snprintf(path, sizeof(path),
             "/Users/%s/Items?ParentId=%s&Limit=%d&Recursive=false&EnableImages=false&EnableUserData=false&EnableTotalRecordCount=false",
             g_cfg.user_id, enc_parent, MAX_ITEMS);

    HttpResponse res;
    set_status("Loading %s...", title && title[0] ? title : "items");
    Result ret = api_get(path, &res);
    if ((R_FAILED(ret) || res.status == HTTP_STATUS_NONE) && parent_id[0]) {
        free_response(&res);
        snprintf(path, sizeof(path),
                 "/Items?UserId=%s&ParentId=%s&Limit=%d&Recursive=false&EnableImages=false&EnableUserData=false&EnableTotalRecordCount=false",
                 g_cfg.user_id, enc_parent, MAX_ITEMS);
        ret = api_get(path, &res);
    }
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        set_http_failure("Could not load items", &res, ret);
        free_response(&res);
        return false;
    }

    parse_items(res.body, g_items, &g_item_count);
    free_response(&res);
    g_selected = 0;
    g_scroll = 0;
    copy_safe(g_current_parent_id, sizeof(g_current_parent_id), parent_id);
    copy_safe(g_screen_title, sizeof(g_screen_title), title && title[0] ? title : "Items");
    g_view = VIEW_ITEMS;
    set_status("%s: %d entries.", g_screen_title, g_item_count);
    return true;
}

static bool is_playable(const MediaItem *item)
{
    if (strcmp(item->type, "Audio") == 0) {
        return false;
    }
    return strcmp(item->type, "Movie") == 0 ||
           strcmp(item->type, "Episode") == 0 ||
           strcmp(item->type, "Video") == 0 ||
           strcmp(item->type, "MusicVideo") == 0 ||
           (!item->is_folder && item->id[0]);
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

static void build_mjpeg_stream_url(const MediaItem *item, char *out, size_t outsz, bool avi_container)
{
    QualityProfile q = quality_profile();
    int fps = mjpeg_target_fps();
    int bitrate = mjpeg_target_bitrate();
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
    if (avi_container) {
        snprintf(out, outsz,
                 "%s/Videos/%s/stream?Container=avi&DeviceId=%s%s%s&VideoCodec=mjpeg&AudioCodec=pcm_s16le&VideoBitrate=%d&AudioBitRate=%d&AudioSampleRate=%d&AudioChannels=1&MaxAudioChannels=1&TranscodingMaxAudioChannels=1&MaxWidth=%d&MaxHeight=%d&Framerate=%d&MaxFramerate=%d&Static=false&EnableAutoStreamCopy=false&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&SubtitleMethod=Encode&Context=Streaming&TranscodeReasons=ContainerNotSupported,VideoCodecNotSupported,AudioCodecNotSupported&ApiKey=%s",
                 g_cfg.server, id, dev, media_q, session_q, bitrate, AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * 16, AUDIO_SAMPLE_RATE, q.width, q.height, fps, fps, token);
    } else {
        snprintf(out, outsz,
                 "%s/Videos/%s/stream?Container=mjpeg&DeviceId=%s%s%s&VideoCodec=mjpeg&VideoBitrate=%d&MaxWidth=%d&MaxHeight=%d&Framerate=%d&MaxFramerate=%d&Static=false&EnableAutoStreamCopy=false&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&SubtitleMethod=Encode&Context=Streaming&TranscodeReasons=ContainerNotSupported,VideoCodecNotSupported&ApiKey=%s",
                 g_cfg.server, id, dev, media_q, session_q, bitrate, q.width, q.height, fps, fps, token);
    }
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

static void build_playback_body(char *out, size_t outsz)
{
    QualityProfile q = quality_profile();
    snprintf(out, outsz,
        "{"
        "\"UserId\":\"%s\","
        "\"StartTimeTicks\":0,"
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
            "\"Name\":\"3dJelly %dp\","
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
        g_cfg.user_id, q.video_bitrate + q.audio_bitrate, g_cfg.quality,
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
    consoleClear();
    printf("3dJelly player\n");
    printf("%s\n\n", g_current.name[0] ? g_current.name : "Video");
    printf("%s\n\n", line ? line : "");
    printf("Quality: %dp\n", g_cfg.quality);
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
    httpcAddRequestHeaderField(context, "User-Agent", "3dJelly/0.1 Nintendo 3DS");
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
    player_console(NULL, "Starting decoder...");
    if (!player_init(&player)) {
        player_free(&player, false);
        gfxExit();
        ui_graphics_init();
        return false;
    }
    mvd_started = true;
    player_console(&player, "Opening Jellyfin stream...");

    httpcContext context;
    u32 status = HTTP_STATUS_NONE;
    Result ret = open_stream_context(&context, url, &status);
    if (R_FAILED(ret)) {
        set_play_status("Stream open failed: HTTP %lu result 0x%08lX.", (unsigned long)status, (unsigned long)ret);
        player_console(&player, g_play_status);
        svcSleepThread(1800000000ULL);
        player_free(&player, mvd_started);
        gfxExit();
        ui_graphics_init();
        return false;
    }

    u8 *chunk = (u8 *)malloc(STREAM_READ_SIZE);
    if (!chunk) {
        set_play_status("Could not allocate stream read buffer.");
    } else {
        set_play_status("Playing. Press B to stop.");
        player_console(&player, g_play_status);
        u64 last_console_ms = osGetTime();
        while (aptMainLoop()) {
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

            u32 read_size = 0;
            ret = httpcDownloadData(&context, chunk, STREAM_READ_SIZE, &read_size);
            if (read_size && !feed_ts_bytes(&player, chunk, read_size)) {
                break;
            }
            if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING) {
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

    httpcCancelConnection(&context);
    httpcCloseContext(&context);

    if (!ok && !g_play_status[0]) {
        set_play_status("Playback failed.");
    }
    player_console(&player, g_play_status);
    svcSleepThread(900000000ULL);
    player_free(&player, mvd_started);
    gfxExit();
    ui_graphics_init();
    return ok;
}

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
    u32 byte_count;
    u32 target_fps;
    u32 target_bitrate;
    u64 next_frame_time_ns;
    u64 frame_interval_ns;
    unsigned last_jpeg_status;
    bool avi_mode;
    bool avi_in_movi;
} MjpegPlayer;

static int min_int(int a, int b)
{
    return a < b ? a : b;
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

static u16 rgb565_from_rgb(u8 r, u8 g, u8 b)
{
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

    memset(pixels, 0, (size_t)info.m_width * (size_t)info.m_height * sizeof(u16));

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

static void draw_rgb565_frame(const u16 *pixels, int img_w, int img_h)
{
    if (!pixels || img_w <= 0 || img_h <= 0) {
        return;
    }

    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) {
        return;
    }

    clear_top_rgb565(0);

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
    if (dst_w == img_w && dst_h == img_h) {
        for (int y = 0; y < img_h; y++) {
            int screen_y = y0 + y;
            const u16 *src = pixels + y * img_w;
            for (int x = 0; x < img_w; x++) {
                int screen_x = x0 + x;
                fb[screen_x * 240 + (239 - screen_y)] = src[x];
            }
        }

        gfxFlushBuffers();
        gfxSwapBuffersGpu();
        gspWaitForVBlank();
        return;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = (y * img_h) / dst_h;
        int screen_y = y0 + y;
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * img_w) / dst_w;
            int screen_x = x0 + x;
            fb[screen_x * 240 + (239 - screen_y)] = pixels[sy * img_w + sx];
        }
    }

    gfxFlushBuffers();
    gfxSwapBuffersGpu();
    gspWaitForVBlank();
}

static void mjpeg_console(const MjpegPlayer *player, const char *line)
{
    consoleClear();
    printf("3dJelly MJPEG player\n");
    printf("%s\n\n", g_current.name[0] ? g_current.name : "Video");
    printf("%s\n\n", line ? line : "");
    printf("Quality: %dp  Decoder: CPU JPEG\n", g_cfg.quality);
    if (player) {
        printf("MJPEG: %lu fps target  %lu kbps\n",
               (unsigned long)player->target_fps,
               (unsigned long)(player->target_bitrate / 1000));
        printf("Frames: %lu  Bytes: %lu\n", (unsigned long)player->frame_count, (unsigned long)player->byte_count);
        printf("JPEG status: %u  skipped: %lu\n", player->last_jpeg_status, (unsigned long)player->decode_fail_count);
        if (!player->audio) {
            printf("Audio: unavailable\n");
        } else if (!player->audio->ndsp_open && !player->audio->failed) {
            printf("Audio: unavailable on this stream\n");
        } else if (player->audio->ndsp_open && player->audio->muted) {
            printf("Audio: muted  press Y\n");
        } else if (player->audio->ndsp_open && player->audio->buffers_submitted > 0) {
            printf("Audio: PCM %d Hz mono  %lu buffers  drop %lu\n",
                   player->audio->sample_rate ? (int)player->audio->sample_rate : AUDIO_SAMPLE_RATE,
                   (unsigned long)player->audio->buffers_submitted,
                   (unsigned long)player->audio->overruns);
        } else if (player->audio && player->audio->ndsp_open) {
            printf("Audio: on  waiting for PCM chunks\n");
        } else if (player->audio && player->audio->failed) {
            printf("Audio: off  result 0x%08lX\n",
                   (unsigned long)player->audio->last_result);
        } else {
            printf("Audio: off\n");
        }
    }
    printf("\nB stop playback  Y mute  START exit app\n");
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

static void audio_queue_sample(AudioPlayer *audio, s16 sample)
{
    if (!audio || !audio->ndsp_open || audio->failed || audio->muted) {
        return;
    }

    audio->pcm_staging[audio->pcm_staging_size++] = (u8)(sample & 0xFF);
    audio->pcm_staging[audio->pcm_staging_size++] = (u8)((sample >> 8) & 0xFF);
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

    Result ret = ndspInit();
    audio->last_result = ret;
    if (R_FAILED(ret)) {
        audio->failed = true;
        return false;
    }
    audio->ndsp_open = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
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
        u64 wait = player->next_frame_time_ns - now;
        if (wait > 20000000ULL) {
            wait = 20000000ULL;
        }
        audio_refill(player->audio);
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

static bool process_avi_video_chunk(MjpegPlayer *player, const u8 *data, size_t size)
{
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

    for (;;) {
        if (player->size < 8) {
            return true;
        }

        if (memcmp(player->buf, "LIST", 4) == 0) {
            if (player->size < 12) {
                return true;
            }
            u32 list_size = read_le32(player->buf + 4);
            if (memcmp(player->buf + 8, "rec ", 4) == 0 || memcmp(player->buf + 8, "movi", 4) == 0) {
                memmove(player->buf, player->buf + 12, player->size - 12);
                player->size -= 12;
                continue;
            }
            size_t total = 8 + (size_t)list_size + (list_size & 1U);
            if (total > player->size) {
                return true;
            }
            memmove(player->buf, player->buf + total, player->size - total);
            player->size -= total;
            continue;
        }

        bool is_video = avi_stream_chunk(player->buf, 'd', 'c') || avi_stream_chunk(player->buf, 'd', 'b');
        bool is_audio = avi_stream_chunk(player->buf, 'w', 'b');
        if (!is_video && !is_audio) {
            size_t next = 1;
            while (next + 8 <= player->size &&
                   memcmp(player->buf + next, "LIST", 4) != 0 &&
                   !avi_stream_chunk(player->buf + next, 'd', 'c') &&
                   !avi_stream_chunk(player->buf + next, 'd', 'b') &&
                   !avi_stream_chunk(player->buf + next, 'w', 'b')) {
                next++;
            }
            memmove(player->buf, player->buf + next, player->size - next);
            player->size -= next;
            continue;
        }

        u32 chunk_size = read_le32(player->buf + 4);
        if (chunk_size > MJPEG_FRAME_CAP - 16) {
            player->size = 0;
            set_play_status("AVI chunk too large; resyncing.");
            return true;
        }
        size_t total = 8 + (size_t)chunk_size + (chunk_size & 1U);
        if (total > player->size) {
            return true;
        }

        const u8 *payload = player->buf + 8;
        if (is_video) {
            if (!process_avi_video_chunk(player, payload, chunk_size)) {
                return false;
            }
        } else {
            audio_queue_pcm(player->audio, payload, chunk_size);
        }

        memmove(player->buf, player->buf + total, player->size - total);
        player->size -= total;
    }
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
    mjpeg_pace_frame(player);
    draw_rgb565_frame(player->pixels, w, h);
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
        if (!process_mjpeg_frame(player, player->buf, frame_len)) {
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

static bool play_mjpeg_stream_url(const char *url, bool avi_container)
{
    if (!url || !url[0]) {
        set_play_status("No MJPEG playback URL.");
        return false;
    }

    ui_graphics_exit();
    gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false);
    consoleInit(GFX_BOTTOM, NULL);
    clear_top_rgb565(0);
    gfxFlushBuffers();
    gfxSwapBuffersGpu();

    MjpegPlayer player;
    memset(&player, 0, sizeof(player));
    player.target_fps = (u32)mjpeg_target_fps();
    player.target_bitrate = (u32)mjpeg_target_bitrate();
    player.frame_interval_ns = 1000000000ULL / (player.target_fps ? player.target_fps : 1);
    player.avi_mode = avi_container;
    player.buf = (u8 *)malloc(MJPEG_FRAME_CAP);
    player.pixels = (u16 *)linearMemAlign(JPEG_PIXELS_CAP * sizeof(u16), 0x40);
    if (!player.buf || !player.pixels) {
        set_play_status("Not enough memory for MJPEG playback.");
        mjpeg_console(&player, g_play_status);
        svcSleepThread(1500000000ULL);
        mjpeg_free(&player);
        gfxExit();
        ui_graphics_init();
        return false;
    }

    mjpeg_console(&player, "Opening Jellyfin MJPEG stream...");

    httpcContext context;
    u32 status = HTTP_STATUS_NONE;
    Result ret = open_stream_context(&context, url, &status);
    if (R_FAILED(ret)) {
        set_play_status("MJPEG stream failed: HTTP %lu result 0x%08lX.", (unsigned long)status, (unsigned long)ret);
        mjpeg_console(&player, g_play_status);
        svcSleepThread(1500000000ULL);
        mjpeg_free(&player);
        gfxExit();
        ui_graphics_init();
        return false;
    }

    AudioPlayer audio;
    memset(&audio, 0, sizeof(audio));
    player.audio = &audio;
    if (avi_container && !audio_start(&audio, NULL)) {
        set_play_status("Audio init failed; playing video only.");
    }

    u8 *chunk = (u8 *)malloc(MJPEG_READ_SIZE);
    bool ok = false;
    if (!chunk) {
        set_play_status("Could not allocate MJPEG read buffer.");
    } else {
        set_play_status("Playing MJPEG. Press B to stop.");
        mjpeg_console(&player, g_play_status);
        u64 last_console_ms = osGetTime();
        while (aptMainLoop()) {
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
            if (down & KEY_Y) {
                if (audio.ndsp_open) {
                    audio.muted = !audio.muted;
                    audio.pcm_staging_size = 0;
                    set_play_status(audio.muted ? "Audio muted." : "Audio on.");
                } else {
                    set_play_status("Audio unavailable on this stream.");
                }
                mjpeg_console(&player, g_play_status);
                last_console_ms = osGetTime();
            }

            audio_refill(player.audio);
            u32 read_size = 0;
            ret = httpcDownloadData(&context, chunk, MJPEG_READ_SIZE, &read_size);
            if (read_size) {
                bool fed = avi_container ? feed_avi_mjpeg_bytes(&player, chunk, read_size)
                                         : feed_mjpeg_bytes(&player, chunk, read_size);
                if (!fed) {
                    break;
                }
            }
            if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING) {
                u64 now_ms = osGetTime();
                if (now_ms - last_console_ms >= 1000) {
                    mjpeg_console(&player, "Playing MJPEG. Press B to stop.");
                    last_console_ms = now_ms;
                }
                continue;
            }
            if (R_FAILED(ret)) {
                set_play_status("MJPEG read failed: 0x%08lX.", (unsigned long)ret);
                break;
            }
            set_play_status("MJPEG reached end of stream.");
            ok = true;
            break;
        }
        free(chunk);
    }

    audio_stop(&audio);
    httpcCancelConnection(&context);
    httpcCloseContext(&context);

    mjpeg_console(&player, g_play_status);
    svcSleepThread(700000000ULL);
    mjpeg_free(&player);
    gfxExit();
    ui_graphics_init();
    return ok;
}

static bool play_current_item_video(void)
{
    if (g_is_new_3ds) {
        set_play_status("New3DS detected: using H.264/MVD playback.");
        if (play_stream_url(g_play_url)) {
            return true;
        }
        if (g_exit_requested) {
            return false;
        }
        set_play_status("MVD unavailable; falling back to MJPEG software playback.");
    } else {
        set_play_status("Old3DS detected: using MJPEG software playback.");
    }

    char mjpeg_url[STREAM_URL_CAP];
    build_mjpeg_stream_url(&g_current, mjpeg_url, sizeof(mjpeg_url), true);
    copy_safe(g_play_url, sizeof(g_play_url), mjpeg_url);
    copy_safe(g_play_method, sizeof(g_play_method), g_is_new_3ds ? "avi-mjpeg-pcm-fallback" : "old3ds-avi-mjpeg-pcm");
    if (play_mjpeg_stream_url(g_play_url, true)) {
        return true;
    }
    if (g_exit_requested) {
        return false;
    }

    build_mjpeg_stream_url(&g_current, mjpeg_url, sizeof(mjpeg_url), false);
    copy_safe(g_play_url, sizeof(g_play_url), mjpeg_url);
    copy_safe(g_play_method, sizeof(g_play_method), g_is_new_3ds ? "raw-mjpeg-fallback" : "old3ds-raw-mjpeg");
    set_play_status("Trying raw MJPEG fallback...");
    return play_mjpeg_stream_url(g_play_url, false);
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

    char path[256];
    char body[4096];
    char enc_id[128];
    url_encode(g_current.id, enc_id, sizeof(enc_id));
    snprintf(path, sizeof(path), "/Items/%s/PlaybackInfo?UserId=%s", enc_id, g_cfg.user_id);
    build_playback_body(body, sizeof(body));

    HttpResponse res;
    snprintf(g_play_status, sizeof(g_play_status), "Requesting %dp transcode info...", g_cfg.quality);
    set_status("%s", g_play_status);
    Result ret = api_post(path, body, true, &res);
    if (R_FAILED(ret) || res.status < 200 || res.status >= 300 || !res.body) {
        format_http_failure(g_play_status, sizeof(g_play_status), "PlaybackInfo failed", &res, ret);
        set_status("%s", g_play_status);
        free_response(&res);
        return false;
    }

    const char *end = res.body + res.size;
    json_get_string_range(res.body, end, "PlaySessionId", g_play_session, sizeof(g_play_session));

    const char *arr = find_key_range(res.body, end, "MediaSources");
    const char *src_start = NULL;
    const char *src_end = NULL;
    g_play_url[0] = 0;
    g_play_method[0] = 0;
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
             "PlaybackInfo OK. Transcode:%s Direct:%s",
             supports_transcoding ? "yes" : "no",
             supports_direct ? "yes" : "no");

    set_status("%s", g_play_status);
    play_current_item_video();
    return true;
}

static void clipped_name(const char *src, char *out, size_t outsz, size_t max_chars)
{
    if (!outsz) {
        return;
    }
    snprintf(out, outsz, "%.*s", (int)(outsz - 1), src ? src : "");
    size_t n = strlen(out);
    if (n > max_chars && outsz > 4) {
        out[max_chars > outsz - 4 ? outsz - 4 : max_chars] = 0;
        strcat(out, "...");
    }
}

static void draw_text(float x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    C2D_Text t;
    C2D_TextParse(&t, g_text, buf);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void draw_text_wrap(float x, float y, float scale, float wrap, u32 color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    C2D_Text t;
    C2D_TextParse(&t, g_text, buf);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0.5f, scale, scale, color, wrap);
}

static void draw_header(const char *title)
{
    C2D_DrawRectSolid(0, 0, 0, 400, 34, COL_PAPER);
    C2D_DrawRectSolid(0, 33, 0, 400, 2, COL_PRIMARY);
    C2D_DrawRectSolid(9, 8, 0, 10, 18, COL_PRIMARY);
    C2D_DrawRectSolid(22, 8, 0, 10, 18, COL_SECONDARY);
    draw_text(42, 8, 0.55f, COL_WHITE, "3dJelly");
    draw_text(136, 9, 0.48f, COL_MUTED, "%s", title);
}

static void draw_bottom_help(const char *line1, const char *line2)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_PAPER);
    C2D_DrawRectSolid(0, 0, 0, 320, 4, COL_PRIMARY);
    draw_text_wrap(12, 15, 0.48f, 296, COL_WHITE, "%s", line1);
    draw_text_wrap(12, 55, 0.43f, 296, COL_MUTED, "%s", line2);
    draw_text_wrap(12, 168, 0.40f, 296, COL_MUTED, "%s", g_status);
}

static void draw_list(MediaItem *items, int count, const char *title)
{
    draw_header(title);

    if (count <= 0) {
        C2D_DrawRectSolid(24, 72, 0, 352, 86, COL_PAPER);
        draw_text(40, 88, 0.55f, COL_WHITE, "No items found");
        draw_text_wrap(40, 116, 0.42f, 310, COL_MUTED, "Refresh with X or go back with B.");
        return;
    }

    if (g_selected < 0) {
        g_selected = 0;
    }
    if (g_selected >= count) {
        g_selected = count - 1;
    }
    if (g_selected < g_scroll) {
        g_scroll = g_selected;
    }
    if (g_selected >= g_scroll + 5) {
        g_scroll = g_selected - 4;
    }

    for (int row = 0; row < 5 && g_scroll + row < count; row++) {
        int idx = g_scroll + row;
        float y = 44.0f + row * 36.0f;
        bool sel = idx == g_selected;
        u32 card = sel ? COL_PRIMARY_DARK : ((idx & 1) ? COL_CARD_2 : COL_CARD);
        C2D_DrawRectSolid(16, y, 0, 368, 30, card);
        C2D_DrawRectSolid(16, y + 29, 0, 368, 1, sel ? COL_SECONDARY : 0x55000000);

        char name[72];
        clipped_name(items[idx].name, name, sizeof(name), 42);
        draw_text(25, y + 6, 0.43f, COL_WHITE, "%s", name);

        const char *kind = items[idx].collection_type[0] ? items[idx].collection_type : items[idx].type;
        draw_text(290, y + 8, 0.35f, COL_MUTED, "%s", kind);
    }

    draw_text(18, 224, 0.36f, COL_MUTED, "%d/%d", g_selected + 1, count);
}

static void draw_setup(void)
{
    draw_header("Setup");
    C2D_DrawRectSolid(18, 48, 0, 364, 146, COL_PAPER);

    const char *labels[] = {"Server", "Username", "Password", "Login"};
    char values[4][256];
    snprintf(values[0], sizeof(values[0]), "%s", g_cfg.server);
    snprintf(values[1], sizeof(values[1]), "%s", g_cfg.username[0] ? g_cfg.username : "not set");
    snprintf(values[2], sizeof(values[2]), "%s", g_cfg.password[0] ? "stored" : "not set");
    snprintf(values[3], sizeof(values[3]), "%s", g_cfg.token[0] ? "refresh token/session" : "authenticate");

    for (int i = 0; i < 4; i++) {
        float y = 58.0f + i * 32.0f;
        if (i == g_setup_row) {
            C2D_DrawRectSolid(28, y - 4, 0, 344, 25, COL_PRIMARY_DARK);
        }
        draw_text(36, y, 0.43f, COL_WHITE, "%s", labels[i]);
        draw_text(130, y, 0.39f, i == g_setup_row ? COL_WHITE : COL_MUTED, "%s", values[i]);
    }

    QualityProfile q = quality_profile();
    draw_text(28, 210, 0.38f, COL_MUTED, "Quality target: %dp (%dx%d, %dk video)", g_cfg.quality, q.width, q.height, q.video_bitrate / 1000);
    draw_text(28, 225, 0.34f, COL_MUTED, "Playback: %s", g_is_new_3ds ? "New3DS H264/MVD + MJPEG fallback" : "Old3DS MJPEG software");
}

static void draw_detail(void)
{
    draw_header("Item");
    C2D_DrawRectSolid(18, 50, 0, 364, 140, COL_PAPER);
    C2D_DrawRectSolid(18, 50, 0, 8, 140, COL_PRIMARY);
    draw_text_wrap(38, 64, 0.62f, 330, COL_WHITE, "%s", g_current.name);
    draw_text(38, 106, 0.43f, COL_MUTED, "Type: %s", g_current.type);
    if (g_current.year) {
        draw_text(38, 128, 0.43f, COL_MUTED, "Year: %d", g_current.year);
    }
    if (g_current.runtime_ticks) {
        unsigned long long minutes = g_current.runtime_ticks / 600000000ULL;
        draw_text(38, 150, 0.43f, COL_MUTED, "Runtime: %llumin", minutes);
    }
    draw_text(38, 174, 0.40f, COL_PRIMARY, "A: request %dp transcode", g_cfg.quality);
}

static void draw_playback(void)
{
    draw_header("Playback");
    QualityProfile q = quality_profile();
    float preview_w = 110.0f + (float)q.height * 0.35f;
    if (preview_w > 250.0f) {
        preview_w = 250.0f;
    }
    C2D_DrawRectSolid(14, 45, 0, 372, 164, COL_PAPER);
    C2D_DrawRectSolid(24, 58, 0, preview_w, 74, COL_CARD);
    C2D_DrawRectSolid(24, 58, 0, (float)(g_frame_counter % (int)preview_w), 4, COL_PRIMARY);
    draw_text(36, 76, 0.55f, COL_WHITE, "%dp", g_cfg.quality);
    draw_text(36, 103, 0.38f, COL_MUTED, "%s", g_play_method[0] ? g_play_method : "stream");

    draw_text_wrap(220, 58, 0.38f, 150, COL_WHITE, "%s", g_current.name);
    draw_text_wrap(24, 145, 0.35f, 344, COL_MUTED, "%s", g_play_status[0] ? g_play_status : "No probe yet.");
    draw_text_wrap(24, 178, 0.31f, 344, COL_MUTED, "%s", g_play_url[0] ? g_play_url : "No URL.");
}

static void draw_tests(void)
{
    draw_header("Render Test");
    QualityProfile q = quality_profile();
    int w = q.width;
    int h = q.height;
    float scale = 1.0f;
    if (w > 360) {
        scale = 360.0f / (float)w;
    }
    if ((float)h * scale > 154.0f) {
        scale = 154.0f / (float)h;
    }
    w = (int)((float)w * scale);
    h = (int)((float)h * scale);
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    float x = (400 - w) / 2.0f;
    float y = (240 - h) / 2.0f + 10.0f;
    if (y < 42) {
        y = 42;
    }
    C2D_DrawRectSolid(x, y, 0, (float)w, (float)h, 0xFF050505);
    for (int i = 0; i < 18; i++) {
        float px = x + (float)((i * 37 + g_frame_counter * 3) % w);
        float py = y + (float)((i * 23 + g_frame_counter * 2) % h);
        C2D_DrawRectSolid(px, py, 0, 18, 10, (i & 1) ? COL_PRIMARY : COL_SECONDARY);
    }
    draw_text(16, 214, 0.38f, COL_MUTED, "Synthetic %dp viewport, frame %u", g_cfg.quality, g_frame_counter);
}

static void render(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(g_text);

    C2D_TargetClear(g_top, COL_BG);
    C2D_SceneBegin(g_top);
    switch (g_view) {
    case VIEW_SETUP:
        draw_setup();
        break;
    case VIEW_LIBRARIES:
        draw_list(g_libraries, g_library_count, "Libraries");
        break;
    case VIEW_ITEMS:
        draw_list(g_items, g_item_count, g_screen_title);
        break;
    case VIEW_DETAIL:
        draw_detail();
        break;
    case VIEW_PLAYBACK:
        draw_playback();
        break;
    case VIEW_TESTS:
        draw_tests();
        break;
    }

    C2D_TargetClear(g_bottom, COL_PAPER);
    C2D_SceneBegin(g_bottom);
    if (g_view == VIEW_SETUP) {
        draw_bottom_help("A edit/select  D-Pad move  L/R quality  START exit",
                         "Server format: http://your-server:8096. HTTPS works with certificate verification disabled for local/self-signed servers.");
    } else if (g_view == VIEW_PLAYBACK) {
        draw_bottom_help("B back  X play again  L/R quality",
                         g_is_new_3ds ? "New3DS uses Jellyfin TS/H.264 with MVD first, then AVI/MJPEG fallback with PCM audio. Y mutes in MJPEG playback."
                                      : "Old3DS defaults to 144p, with 240p/360p/480p available for testing. Y mutes in MJPEG playback.");
    } else if (g_view == VIEW_TESTS) {
        draw_bottom_help("SELECT return  L/R quality  START exit",
                         "Render-only stress view for comparing 144p, 240p, 360p, and 480p viewport cost.");
    } else {
        draw_bottom_help(g_view == VIEW_LIBRARIES ? "A open  X refresh  Y setup  L/R quality  SELECT test" : "A open/play  B back  X refresh  L/R quality",
                         "Jellyfin-style native UI using dark, cyan, and purple theme colors from Jellyfin Web.");
    }

    C3D_FrameEnd(0);
}

static void ui_graphics_init(void)
{
    if (g_ui_ready) {
        return;
    }

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_text = C2D_TextBufNew(8192);
    g_ui_ready = true;
}

static void ui_graphics_exit(void)
{
    if (!g_ui_ready) {
        return;
    }

    if (g_text) {
        C2D_TextBufDelete(g_text);
        g_text = NULL;
    }
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    g_top = NULL;
    g_bottom = NULL;
    g_ui_ready = false;
}

static void move_selection(int delta, int count)
{
    if (count <= 0) {
        return;
    }
    g_selected += delta;
    if (g_selected < 0) {
        g_selected = count - 1;
    }
    if (g_selected >= count) {
        g_selected = 0;
    }
}

static void change_quality(int dir)
{
    int idx = quality_index(g_cfg.quality);
    if (idx < 0) {
        idx = quality_index(default_quality());
    }
    if (idx < 0) {
        idx = 0;
    }

    int count = quality_count();
    idx = (idx + (dir >= 0 ? 1 : -1) + count) % count;
    g_cfg.quality = QUALITY_LEVELS[idx];
    save_config();
    set_status("Quality target set to %dp.", g_cfg.quality);
}

static void handle_setup(u32 down)
{
    if (down & KEY_DOWN) {
        g_setup_row = (g_setup_row + 1) % 4;
    }
    if (down & KEY_UP) {
        g_setup_row = (g_setup_row + 3) % 4;
    }
    if (down & KEY_L) {
        change_quality(-1);
    }
    if (down & KEY_R) {
        change_quality(1);
    }
    if (down & KEY_B) {
        if (g_cfg.token[0] && g_cfg.user_id[0]) {
            load_libraries();
        }
    }
    if (down & KEY_A) {
        if (g_setup_row == 0 && edit_text("Jellyfin server URL", g_cfg.server, sizeof(g_cfg.server), false)) {
            normalize_server_url(g_cfg.server);
            save_config();
        } else if (g_setup_row == 1 && edit_text("Jellyfin username", g_cfg.username, sizeof(g_cfg.username), false)) {
            g_cfg.token[0] = 0;
            g_cfg.user_id[0] = 0;
            save_config();
        } else if (g_setup_row == 2 && edit_text("Jellyfin password", g_cfg.password, sizeof(g_cfg.password), true)) {
            g_cfg.token[0] = 0;
            g_cfg.user_id[0] = 0;
            save_config();
        } else if (g_setup_row == 3) {
            if (login_jellyfin()) {
                load_libraries();
            }
        }
    }
}

static void handle_common(u32 down)
{
    if (down & KEY_L) {
        change_quality(-1);
    }
    if (down & KEY_R) {
        change_quality(1);
    }
    if (down & KEY_SELECT) {
        g_view = (g_view == VIEW_TESTS) ? VIEW_LIBRARIES : VIEW_TESTS;
    }
}

static void handle_input(u32 down)
{
    if (g_view == VIEW_SETUP) {
        handle_setup(down);
        return;
    }

    handle_common(down);

    if (g_view == VIEW_LIBRARIES) {
        if (down & KEY_DOWN) {
            move_selection(1, g_library_count);
        }
        if (down & KEY_UP) {
            move_selection(-1, g_library_count);
        }
        if (down & KEY_X) {
            load_libraries();
        }
        if (down & KEY_Y) {
            g_view = VIEW_SETUP;
            set_status("Setup opened.");
        }
        if ((down & KEY_A) && g_library_count > 0) {
            push_nav("", "Libraries");
            load_items_for_parent(g_libraries[g_selected].id, g_libraries[g_selected].name);
        }
    } else if (g_view == VIEW_ITEMS) {
        if (down & KEY_DOWN) {
            move_selection(1, g_item_count);
        }
        if (down & KEY_UP) {
            move_selection(-1, g_item_count);
        }
        if (down & KEY_X) {
            if (g_current_parent_id[0]) {
                load_items_for_parent(g_current_parent_id, g_screen_title);
            }
        }
        if (down & KEY_B) {
            pop_nav();
        }
        if ((down & KEY_A) && g_item_count > 0) {
            MediaItem *item = &g_items[g_selected];
            if (is_playable(item)) {
                g_current = *item;
                g_return_view = VIEW_ITEMS;
                probe_playback();
            } else {
                push_nav(g_current_parent_id, g_screen_title);
                load_items_for_parent(item->id, item->name);
            }
        }
    } else if (g_view == VIEW_DETAIL) {
        if (down & KEY_B) {
            g_view = VIEW_ITEMS;
        }
        if (down & KEY_A) {
            g_return_view = VIEW_DETAIL;
            probe_playback();
        }
    } else if (g_view == VIEW_PLAYBACK) {
        if (down & KEY_B) {
            g_view = g_return_view;
        }
        if (down & KEY_X) {
            probe_playback();
        }
    } else if (g_view == VIEW_TESTS) {
        if (down & KEY_B) {
            g_view = g_cfg.token[0] ? VIEW_LIBRARIES : VIEW_SETUP;
        }
    }
}

int main(void)
{
    ui_graphics_init();
    detect_hardware();
    httpcInit(4 * 1024 * 1024);

    load_config();
    apply_hardware_defaults();
    if (g_cfg.token[0] && g_cfg.user_id[0]) {
        if (!load_libraries()) {
            g_view = VIEW_SETUP;
        }
    } else {
        g_view = VIEW_SETUP;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (g_exit_requested || (down & KEY_START)) {
            break;
        }
        handle_input(down);
        g_frame_counter++;
        render();
    }

    save_config();
    httpcExit();
    ui_graphics_exit();
    return 0;
}

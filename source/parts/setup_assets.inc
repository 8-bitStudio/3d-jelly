/* UI-owned asynchronous requests: no network waits in the drawing path and no
   concurrent access to picojpeg's global decoder state. */
#define SETUP_IMAGE_BYTES (512 * 256 * sizeof(u16))
#define SETUP_DOWNLOAD_CAP (8 * 1024 * 1024)
#define SETUP_CACHE_PATH CONFIG_DIR "/login-wallpaper.bin"
static CURLM *g_setup_multi;
static CURL *g_setup_http;
static char *g_setup_download;
static size_t g_setup_download_size, g_setup_download_capacity;
static bool g_setup_check_server, g_setup_resume_session;
static char g_setup_request_server[256];
static u64 g_setup_retry_at;
static int g_setup_asset_attempts;
static u16 *g_setup_pixels;
static pjpeg_image_info_t g_setup_jpeg;
static JpegMemorySource g_setup_jpeg_source;
static int g_setup_mcu, g_setup_crop_w, g_setup_crop_h, g_setup_crop_x, g_setup_crop_y;
typedef struct { u32 r, g, b, count; } SetupPixelSum;
static SetupPixelSum *g_setup_sums;
static int g_setup_sample_x[8192];

static void setup_close_http(void)
{
    if (g_setup_http) {
        if (g_setup_multi) curl_multi_remove_handle(g_setup_multi, g_setup_http);
        curl_easy_cleanup(g_setup_http);
        g_setup_http = NULL;
    }
    if (g_setup_multi) curl_multi_cleanup(g_setup_multi);
    g_setup_multi = NULL;
}

static void setup_cancel_http(void)
{
    setup_close_http();
    free(g_setup_download);
    g_setup_download = NULL;
    g_setup_download_size = g_setup_download_capacity = 0;
    free(g_setup_pixels);
    g_setup_pixels = NULL;
    free(g_setup_sums);
    g_setup_sums = NULL;
}

static void setup_free_wallpaper(void)
{
    setup_cancel_http();
    if (g_setup_wallpaper_ready) C3D_TexDelete(&g_setup_wallpaper);
    g_setup_wallpaper_ready = false;
    g_setup_wallpaper_server[0] = 0;
    g_setup_asset_attempts = 0;
    g_setup_retry_at = 0;
}

static bool setup_upload_wallpaper(const u16 *pixels)
{
    u16 *tiled = malloc(SETUP_IMAGE_BYTES);
    if (!tiled) { setup_log("Wallpaper: no memory for texture staging"); return false; }
    if (!g_setup_wallpaper_ready && !C3D_TexInit(&g_setup_wallpaper, 512, 256, GPU_RGB565)) {
        free(tiled);
        setup_log("Wallpaper: GPU texture allocation failed");
        return false;
    }
    browse_swizzle_rgb565(pixels, tiled, 512, 256);
    C3D_TexSetFilter(&g_setup_wallpaper, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_setup_wallpaper, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexUpload(&g_setup_wallpaper, tiled);
    C3D_TexFlush(&g_setup_wallpaper);
    free(tiled);
    g_setup_wallpaper_ready = true;
    return true;
}

static void setup_read_cache(void)
{
    FILE *f = fopen(SETUP_CACHE_PATH, "rb");
    if (!f) return;
    char magic[8], server[256];
    if (fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
        !memcmp(magic, "3DJBG02", 8) && fread(server, 1, sizeof(server), f) == sizeof(server) &&
        memchr(server, 0, sizeof(server)) && !strcmp(server, g_cfg.server)) {
        u16 *pixels = malloc(SETUP_IMAGE_BYTES);
        if (pixels && fread(pixels, 1, SETUP_IMAGE_BYTES, f) == SETUP_IMAGE_BYTES &&
            setup_upload_wallpaper(pixels))
            setup_log("Wallpaper: loaded SD cache");
        free(pixels);
    }
    fclose(f);
}

static void setup_write_cache(const u16 *pixels)
{
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(SETUP_CACHE_PATH ".tmp", "wb");
    if (!f) return;
    char server[256] = {0};
    copy_safe(server, sizeof(server), g_cfg.server);
    bool ok = fwrite("3DJBG02", 1, 8, f) == 8 &&
              fwrite(server, 1, sizeof(server), f) == sizeof(server) &&
              fwrite(pixels, 1, SETUP_IMAGE_BYTES, f) == SETUP_IMAGE_BYTES;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(SETUP_CACHE_PATH ".tmp", SETUP_CACHE_PATH) != 0) {
        /* FAT implementations may refuse to rename over an existing file. */
        remove(SETUP_CACHE_PATH);
        rename(SETUP_CACHE_PATH ".tmp", SETUP_CACHE_PATH);
    }
}

static size_t setup_receive(void *data, size_t size, size_t count, void *unused)
{
    (void)unused;
    size_t n = size * count;
    if (!n) return 0;
    size_t limit = g_setup_check_server ? 16384 : SETUP_DOWNLOAD_CAP;
    if (n > limit || g_setup_download_size > limit - n) return 0;
    size_t needed = g_setup_download_size + n + 1;
    if (needed > g_setup_download_capacity) {
        size_t capacity = g_setup_download_capacity ? g_setup_download_capacity : 16384;
        while (capacity < needed) capacity *= 2;
        if (capacity > limit + 1) capacity = limit + 1;
        char *next = realloc(g_setup_download, capacity);
        if (!next) return 0;
        g_setup_download = next;
        g_setup_download_capacity = capacity;
    }
    memcpy(g_setup_download + g_setup_download_size, data, n);
    g_setup_download_size += n;
    g_setup_download[g_setup_download_size] = 0;
    return n;
}

static bool setup_begin_http(bool check_server)
{
    setup_cancel_http();
    g_setup_check_server = check_server;
    if (!g_cfg.server[0] || !curl_http_runtime_init()) return false;
    g_setup_multi = curl_multi_init();
    g_setup_http = curl_easy_init();
    if (!g_setup_multi || !g_setup_http) { setup_cancel_http(); return false; }
    copy_safe(g_setup_request_server, sizeof(g_setup_request_server), g_cfg.server);
    char url[768];
    /* Splashscreen ignores resize parameters; scale locally when decoding. */
    build_url(url, sizeof(url), check_server ? "/System/Info/Public" :
              "/Branding/Splashscreen?format=jpg");
    curl_easy_setopt(g_setup_http, CURLOPT_URL, url);
    curl_easy_setopt(g_setup_http, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_setup_http, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(g_setup_http, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(g_setup_http, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(g_setup_http, CURLOPT_USERAGENT, "3dJelly/" APP_VERSION);
    curl_easy_setopt(g_setup_http, CURLOPT_CONNECTTIMEOUT_MS, 1800L);
    curl_easy_setopt(g_setup_http, CURLOPT_TIMEOUT_MS, check_server ? 3000L : 15000L);
    /* Match the existing application's TLS policy. */
    curl_easy_setopt(g_setup_http, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(g_setup_http, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(g_setup_http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_setup_http, CURLOPT_WRITEFUNCTION, setup_receive);
    if (curl_multi_add_handle(g_setup_multi, g_setup_http) != CURLM_OK) {
        setup_cancel_http();
        return false;
    }
    return true;
}

static bool setup_begin_decode(void)
{
    g_setup_jpeg_source = (JpegMemorySource){(u8 *)g_setup_download, g_setup_download_size, 0};
    memset(&g_setup_jpeg, 0, sizeof(g_setup_jpeg));
    unsigned status = pjpeg_decode_init(&g_setup_jpeg, jpeg_need_bytes, &g_setup_jpeg_source, 0);
    if (status ||
        g_setup_jpeg.m_width <= 0 || g_setup_jpeg.m_height <= 0 ||
        g_setup_jpeg.m_width > 8192 || g_setup_jpeg.m_height > 8192) {
        setup_log("Wallpaper: JPEG header failed status=%u size=%dx%d", status,
                  g_setup_jpeg.m_width, g_setup_jpeg.m_height);
        return false;
    }
    setup_log("Wallpaper: full-detail decode %dx%d", g_setup_jpeg.m_width,
              g_setup_jpeg.m_height);
    g_setup_crop_w = g_setup_jpeg.m_width;
    g_setup_crop_h = g_setup_jpeg.m_height;
    if (g_setup_crop_w * 240 > g_setup_crop_h * 400) g_setup_crop_w = g_setup_crop_h * 400 / 240;
    else g_setup_crop_h = g_setup_crop_w * 240 / 400;
    if (g_setup_crop_w < 1 || g_setup_crop_h < 1) return false;
    g_setup_crop_x = (g_setup_jpeg.m_width - g_setup_crop_w) / 2;
    g_setup_crop_y = (g_setup_jpeg.m_height - g_setup_crop_h) / 2;
    g_setup_mcu = 0;
    g_setup_pixels = calloc(1, SETUP_IMAGE_BYTES);
    if (!g_setup_pixels) setup_log("Wallpaper: no memory for decoded pixels");
    /* Average all source pixels in each output footprint. Point sampling and
       enlarging a DC-only thumbnail both alias badly on small poster text.
       Fixed-size sums avoid allocating a full-resolution decoded image. */
    if (g_setup_pixels && g_setup_crop_w >= 400 && g_setup_crop_h >= 240) {
        g_setup_sums = calloc(400 * 240, sizeof(*g_setup_sums));
        if (!g_setup_sums) {
            setup_log("Wallpaper: no memory for smooth downsampling");
            free(g_setup_pixels);
            g_setup_pixels = NULL;
            return false;
        }
        for (int x = 0; x < g_setup_crop_w; x++)
            g_setup_sample_x[x] = x * 400 / g_setup_crop_w;
    }
    return g_setup_pixels != NULL;
}

/* A bounded slice per frame keeps touch, exit and server switching responsive. */
static void setup_decode_step(void)
{
    pjpeg_image_info_t *info = &g_setup_jpeg;
    int cw = g_setup_crop_w, ch = g_setup_crop_h, cx = g_setup_crop_x, cy = g_setup_crop_y;
    int total = info->m_MCUSPerRow * info->m_MCUSPerCol;
    u64 until = osGetTime() + 4;
    do {
        unsigned status = pjpeg_decode_mcu();
        if (status) {
            setup_log("Wallpaper: JPEG block %d failed status=%u", g_setup_mcu, status);
            setup_cancel_http();
            set_status("Wallpaper decode failed (%u). Sign-in is still available.", status);
            return;
        }
        int ox = (g_setup_mcu % info->m_MCUSPerRow) * info->m_MCUWidth;
        int oy = (g_setup_mcu / info->m_MCUSPerRow) * info->m_MCUHeight;
        if (g_setup_sums) {
            int left = clamp_int(cx - ox, 0, info->m_MCUWidth);
            int right = clamp_int(cx + cw - ox, 0, info->m_MCUWidth);
            int top = clamp_int(cy - oy, 0, info->m_MCUHeight);
            int bottom = clamp_int(cy + ch - oy, 0, info->m_MCUHeight);
            for (int sy = top; sy < bottom; sy++) {
                SetupPixelSum *row = g_setup_sums + ((oy + sy - cy) * 240 / ch) * 400;
                for (int sx = left; sx < right; sx++) {
                    int offset = (sx / 8) * 64 + (sy / 8) * 128 + (sy % 8) * 8 + sx % 8;
                    u8 r = info->m_pMCUBufR[offset];
                    SetupPixelSum *sum = &row[g_setup_sample_x[ox + sx - cx]];
                    sum->r += r;
                    sum->g += info->m_scanType == PJPG_GRAYSCALE ? r : info->m_pMCUBufG[offset];
                    sum->b += info->m_scanType == PJPG_GRAYSCALE ? r : info->m_pMCUBufB[offset];
                    sum->count++;
                }
            }
        } else {
            int x0 = clamp_int(((ox - cx) * 400 + cw - 1) / cw, 0, 400);
            int x1 = clamp_int(((ox + info->m_MCUWidth - cx) * 400 + cw - 1) / cw, 0, 400);
            int y0 = clamp_int(((oy - cy) * 240 + ch - 1) / ch, 0, 240);
            int y1 = clamp_int(((oy + info->m_MCUHeight - cy) * 240 + ch - 1) / ch, 0, 240);
            for (int y = y0; y < y1; y++) {
                int sy = cy + y * ch / 240 - oy;
                for (int x = x0; x < x1; x++) {
                    int sx = cx + x * cw / 400 - ox;
                    int offset = (sx / 8) * 64 + (sy / 8) * 128 + (sy % 8) * 8 + sx % 8;
                    u8 r = info->m_pMCUBufR[offset];
                    u8 g = info->m_scanType == PJPG_GRAYSCALE ? r : info->m_pMCUBufG[offset];
                    u8 b = info->m_scanType == PJPG_GRAYSCALE ? r : info->m_pMCUBufB[offset];
                    g_setup_pixels[y * 512 + x] = rgb565_from_rgb(r, g, b);
                }
            }
        }
        g_setup_mcu++;
    } while (g_setup_mcu < total && osGetTime() < until);
    if (g_setup_mcu == total) {
        if (g_setup_sums) {
            for (int y = 0; y < 240; y++) {
                for (int x = 0; x < 400; x++) {
                    SetupPixelSum *s = &g_setup_sums[y * 400 + x];
                    if (s->count)
                        g_setup_pixels[y * 512 + x] = rgb565_from_rgb(
                            (s->r + s->count / 2) / s->count,
                            (s->g + s->count / 2) / s->count,
                            (s->b + s->count / 2) / s->count);
                }
            }
        }
        bool uploaded = setup_upload_wallpaper(g_setup_pixels);
        if (uploaded) {
            setup_write_cache(g_setup_pixels);
            setup_log("Wallpaper: decoded and uploaded successfully");
        } else set_status("Wallpaper texture unavailable. Sign-in is still available.");
        setup_cancel_http();
        if (uploaded) g_setup_asset_attempts = 3;
    }
}

static void setup_tick_assets(void)
{
    if (g_setup_choose_server) { setup_cancel_http(); return; }
    if (g_setup_http) {
        int running = 0;
        CURLMcode result = curl_multi_perform(g_setup_multi, &running);
        int remaining = 0;
        CURLMsg *msg = curl_multi_info_read(g_setup_multi, &remaining);
        if (!msg && result == CURLM_OK) return;
        long status = 0;
        long socket_error = 0;
        curl_easy_getinfo(g_setup_http, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(g_setup_http, CURLINFO_OS_ERRNO, &socket_error);
        CURLcode code = msg && msg->msg == CURLMSG_DONE ? msg->data.result : CURLE_RECV_ERROR;
        setup_log("%s: curl=%d multi=%d http=%ld socket=%ld bytes=%lu",
                  g_setup_check_server ? "Server check" : "Wallpaper", (int)code,
                  (int)result, status, socket_error, (unsigned long)g_setup_download_size);
        bool ok = code == CURLE_OK && status == 200;
        setup_close_http();
        if (strcmp(g_setup_request_server, g_cfg.server)) { setup_cancel_http(); return; }
        if (g_setup_check_server) {
            char id[96];
            ok = ok && g_setup_download && json_get_string_range(g_setup_download,
                g_setup_download + g_setup_download_size, "Id", id, sizeof(id)) && id[0];
            setup_cancel_http();
            if (!ok) {
                g_setup_resume_session = false;
                setup_start_scan();
                set_status("Saved server unavailable. Searching your Wi-Fi...");
                return;
            }
            if (g_setup_resume_session) {
                g_setup_resume_session = false;
                if (load_libraries()) return;
                g_view = VIEW_SETUP;
            } else set_status("Enter your User and Pass, then Sign In.");
        } else {
            if (ok && setup_begin_decode()) return;
            setup_cancel_http();
            g_setup_retry_at = osGetTime() + 5000;
            if (!g_setup_wallpaper_ready) {
                if (!status)
                    set_status("Cannot reach server (%d). Choose Change Server to search Wi-Fi.", (int)code);
                else if (ok) set_status("Wallpaper image could not be decoded. Sign-in is still available.");
                else set_status("Sign-in is ready. Server wallpaper unavailable (HTTP %ld).", status);
            }
        }
    }
    if (g_setup_pixels) { setup_decode_step(); return; }
    if (!g_cfg.server[0]) return;
    if (strcmp(g_setup_wallpaper_server, g_cfg.server)) {
        setup_free_wallpaper();
        copy_safe(g_setup_wallpaper_server, sizeof(g_setup_wallpaper_server), g_cfg.server);
        setup_read_cache();
    }
    if (g_setup_asset_attempts < 3 && osGetTime() >= g_setup_retry_at) {
        g_setup_asset_attempts++;
        g_setup_retry_at = osGetTime() + 5000;
        setup_begin_http(false);
    }
}

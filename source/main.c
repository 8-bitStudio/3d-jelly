#include <3ds.h>
#include <citro2d.h>
#include <curl/curl.h>
#include "picojpeg.h"
#include "pl_mpeg.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define APP_VERSION "0.4.2"
#define CONFIG_DIR "sdmc:/3dJelly"
#define CONFIG_PATH "sdmc:/3dJelly/config.ini"

#define MAX_LIBRARIES 64
#define MAX_ITEMS 2048
#define ITEM_PAGE_SIZE 100
#define MAX_STACK 8
#define HTTP_CAP (1024 * 1024)
#define HTTP_STATUS_NONE 0xFFFFFFFFu
#define HTTP_STATUS_TIMEOUT_NS 15000000000ULL
#define HTTP_REDIRECT_LIMIT 6
#define HTTP_TLS_VERIFY_FAILED_RESULT ((Result)0xD8A0A03C)
#define STREAM_READ_TIMEOUT_NS 100000000ULL
#define STREAM_READER_EMPTY_SLEEP_NS 2500000ULL
#define EXIT_POLL_SLEEP_NS 50000000ULL
#define STREAM_URL_CAP 2048
#define STREAM_READ_SIZE (188 * 16)
#define H264_BUFFER_CAP (1024 * 1024)
#define MVD_IN_CAP (1024 * 1024)
#define MVD_OUT_CAP (1024 * 1024)
#define MJPEG_READ_SIZE 8192
#define STREAM_READER_BUFFER_COUNT 256
#define SETTINGS_BUFFER_KB_DEFAULT 2048
#define SETTINGS_BUFFER_KB_MIN 1024
#define SETTINGS_BUFFER_KB_MAX 4096
#define SETTINGS_BUFFER_KB_STEP 1024
#define STREAM_READER_PREBUFFER_CHUNKS 64
#define STREAM_READER_PREBUFFER_MS 3000
#define STREAM_READER_STACK_SIZE (16 * 1024)
#define STREAM_READER_CPU_TIME_LIMIT 30
#define MPEG1_STREAM_BUFFER_CAP (384 * 1024)
#define MJPEG_FRAME_CAP (1024 * 1024)
#define MJPEG_STREAM_OPEN_RETRIES 3
#define MJPEG_STREAM_RETRY_SLEEP_NS 450000000ULL
#define MJPEG_STREAM_READ_RECONNECT_FRAMES 6
#define JPEG_PIXELS_CAP (854 * 480)
#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_SAMPLE_RATE_DEFAULT_OLD3DS 40000
#define AUDIO_SAMPLE_RATE_DEFAULT_NEW3DS 44100
#define AUDIO_CHANNELS 1
#define AUDIO_WAVEBUF_COUNT 6
#define AUDIO_WAVEBUF_SAMPLES 1024
#define AUDIO_TARGET_QUEUED_WAVEBUFS 4
#define AUDIO_READ_SIZE 4096
#define AUDIO_PCM_BUFFER_BYTES (AUDIO_WAVEBUF_SAMPLES * AUDIO_CHANNELS * sizeof(s16))
#define VOLUME_DEFAULT_PERCENT 100
#define VOLUME_MIN_PERCENT 0
#define VOLUME_MAX_PERCENT 300
#define VOLUME_STEP_PERCENT 10
#define VOLUME_OSD_MS 1800
#define AUDIO_LIMIT_KNEE 24576
#define AUDIO_LIMIT_HEADROOM (32767 - AUDIO_LIMIT_KNEE)
#define AUDIO_LOW_WAVEBUFS 2
#define SEEK_STEP_SECONDS 10
#define SEEK_FAST_SECONDS 30
#define SEEK_FASTER_SECONDS 60
#define SEEK_REPEAT_DELAY_MS 360
#define SEEK_REPEAT_FAST_MS 260
#define SEEK_REPEAT_FASTER_MS 180
#define SEEK_COMMIT_DELAY_MS 140
#define SEEK_FAST_AFTER_MS 1800
#define SEEK_FASTER_AFTER_MS 4500
#define SEEK_OSD_MS 1900
#define SEEK_OSD_FADE_MS 650
#define QUALITY_OSD_MS 1900
#define QUALITY_OSD_FADE_MS 650
#define BOTTOM_DIM_DEFAULT_SECONDS 60
#define BOTTOM_DIM_TARGET_PERCENT 31
#define BOTTOM_DIM_FADE_MS 900
#define BOTTOM_DIM_FADE_REDRAW_MS 75
#define NEXT_EPISODE_COUNTDOWN_SECONDS 5
#define NEXT_EPISODE_FADE_IN_MS 1000
#define PLAYBACK_REPORT_INTERVAL_MS 5000
#define PLAYBACK_REPORT_QUEUE_COUNT 6
#define PLAYBACK_REPORT_PATH_CAP 96
#define PLAYBACK_REPORT_BODY_CAP 1024
#define PLAYBACK_REPORT_STACK_SIZE (12 * 1024)
#define PLAYBACK_REPORT_TIMEOUT_MS 280
#define REMOTE_MESSAGE_OSD_MS 5000
#define REMOTE_MESSAGE_OSD_FADE_MS 650
#define WEBSOCKET_SOC_BUFFER_SIZE (1024 * 1024)
#define CURL_SOC_BUFFER_SIZE (1024 * 1024)
#define WEBSOCKET_RECV_CAP 4096
#define WEBSOCKET_MESSAGE_CAP 2048
#define TICKS_PER_SECOND 10000000ULL

typedef enum {
    VIEW_SETUP,
    VIEW_SETTINGS,
    VIEW_LIBRARIES,
    VIEW_ITEMS,
    VIEW_SEARCH,
    VIEW_DETAIL,
    VIEW_PLAYBACK
} View;

typedef enum {
    PLAYBACK_MODE_AUTO,
    PLAYBACK_MODE_H264,
    PLAYBACK_MODE_MJPEG
} PlaybackMode;

typedef enum {
    LANG_EN,
    LANG_AF,
    LANG_ES,
    LANG_JA,
    LANG_FR,
    LANG_DE,
    LANG_IT,
    LANG_RU,
    LANG_NL,
    LANG_PT,
    LANG_PL,
    LANG_KO,
    LANG_ZH_HANS,
    LANG_ZH_HANT,
    LANG_ID,
    LANG_TR,
    LANG_SV,
    LANG_COUNT
} AppLanguage;

typedef enum {
    MJPEG_PLAY_FAILED,
    MJPEG_PLAY_OK,
    MJPEG_PLAY_ENDED,
    MJPEG_PLAY_RESTART
} MjpegPlayResult;

typedef struct {
    char id[80];
    char name[128];
    char type[40];
    char collection_type[40];
    char location_type[32];
    char series_id[80];
    char season_id[80];
    char overview[256];
    bool is_folder;
    bool is_missing;
    bool is_virtual_item;
    bool is_place_holder;
    int year;
    int index_number;
    int parent_index_number;
    int media_source_count;
    int child_count;
    int recursive_item_count;
    unsigned long long runtime_ticks;
} MediaItem;

typedef struct {
    char parent_id[80];
    char title[96];
    char parent_type[40];
    char series_id[80];
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
    int quality; /* 144, 240, 241 (Old3DS 240HQ), 360, or 480 */
    int playback_mode;
    int stream_buffer_kb;
    int audio_sample_rate;
    int volume_percent;
    int bottom_dim_seconds;
    int language;
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

typedef struct {
    char path[PLAYBACK_REPORT_PATH_CAP];
    char body[PLAYBACK_REPORT_BODY_CAP];
} PlaybackReportMessage;

static Config g_cfg;
static View g_view = VIEW_SETUP;
static C3D_RenderTarget *g_top;
static C3D_RenderTarget *g_bottom;
static C2D_TextBuf g_text;
static C2D_Font g_font_kor;
static C2D_Font g_font_chn;
static C2D_Font g_font_twn;
static bool g_ui_ready;
static bool g_cfgu_ready;

static MediaItem g_libraries[MAX_LIBRARIES];
static int g_library_count;
static MediaItem g_items[MAX_ITEMS];
static int g_item_count;
static MediaItem g_current;
static NavFrame g_stack[MAX_STACK];
static int g_stack_depth;
static int g_selected;
static int g_scroll;
static int g_setup_row;
static int g_settings_row;
static int g_settings_scroll;
static int g_settings_confirm_row = -1;
static u64 g_settings_confirm_until_ms;
static u64 g_settings_popup_until_ms;
static char g_settings_popup[128];
static bool g_language_select_open;
static int g_language_select_row;
static int g_language_select_scroll;
static char g_screen_title[96] = "Libraries";
static char g_current_parent_id[80];
static char g_current_parent_type[40];
static char g_current_parent_series_id[80];
static char g_search_query[96];
static View g_search_return_view = VIEW_LIBRARIES;
static char g_search_return_parent_id[80];
static char g_search_return_title[96];
static char g_search_return_parent_type[40];
static char g_search_return_series_id[80];
static int g_search_return_selected;
static int g_search_return_scroll;
static char g_status[192] = "Press Y to configure a Jellyfin server.";
static char g_play_url[STREAM_URL_CAP];
static char g_play_method[64];
static char g_play_session[96];
static char g_play_media_source_id[128];
static char g_play_status[192];
static View g_return_view = VIEW_ITEMS;
static View g_settings_return_view = VIEW_LIBRARIES;
static u64 g_mjpeg_resume_ticks;
static u32 g_stream_switch_serial;
static u64 g_seek_osd_until_ms;
static u64 g_seek_osd_target_ticks;
static int g_seek_osd_delta_seconds;
static u64 g_seek_repeat_ready_ms;
static bool g_seek_osd_pending;
static u64 g_quality_osd_until_ms;
static bool g_quality_osd_pending;
static u64 g_bottom_dim_activity_ms;
static u64 g_bottom_dim_fade_start_ms;
static u64 g_bottom_dim_last_redraw_ms;
static bool g_bottom_dim_active;
static bool g_bottom_dim_fade_complete;
static bool g_bottom_dim_redraw_pending;
static bool g_playback_restart_in_progress;
static bool g_playback_ended_naturally;
static bool g_autoplay_cancelled;
static bool g_autoplay_next_ready;
static bool g_autoplay_next_available;
static MediaItem g_autoplay_next_item;
static bool g_session_capabilities_reported;
static bool g_playback_report_active;
static u64 g_playback_last_report_ms;
static bool g_playback_report_sync_ready;
static bool g_playback_report_worker_running;
static bool g_playback_report_worker_stop;
static Thread g_playback_report_thread;
static LightLock g_playback_report_lock;
static CondVar g_playback_report_cv;
static PlaybackReportMessage g_playback_report_queue[PLAYBACK_REPORT_QUEUE_COUNT];
static int g_playback_report_queue_read;
static int g_playback_report_queue_write;
static int g_playback_report_queue_count;
static char g_remote_status[96] = "REMOTE NOT STARTED";
static Result g_remote_last_result;
static int g_remote_last_errno;
static int g_remote_last_http_status;
static u64 g_remote_message_until_ms;
static char g_remote_message_header[64];
static char g_remote_message_text[192];
static unsigned g_frame_counter;
static volatile bool g_exit_requested;
static volatile bool g_system_close_requested;
static bool g_is_new_3ds;
static bool g_http_ready;
static bool g_curl_soc_ready;
static bool g_curl_global_ready;
static u32 *g_curl_soc_buffer;
static Result g_curl_init_result;
static bool g_performance_mode_checked;
static bool g_core1_reader_available;
static Result g_performance_mode_result;
static aptHookCookie g_apt_hook;
static bool g_apt_hooked;
static bool g_volume_save_pending;
static u16 g_rgb565_r[256];
static u16 g_rgb565_g[256];
static u16 g_rgb565_b[256];

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
static bool app_system_closing(void);
static bool app_keep_running(void);
static bool app_wait_or_exit(u64 ns);
static void playback_graphics_exit(void);
static void save_config(void);
static void change_quality(int dir);
static void build_url(char *out, size_t outsz, const char *path);
static bool play_stream_url(const char *url);
static MjpegPlayResult play_mjpeg_stream_url(const char *url, bool avi_container, u64 start_time_ticks);
static MjpegPlayResult play_mpeg1_stream_url(const char *url, u64 start_time_ticks);
static bool play_current_item_video(void);
static u64 monotonic_ns(void);
static u64 clamp_media_ticks(u64 ticks);
static bool remote_http_post_json_quick(const char *path, const char *body, int timeout_ms);

static const int QUALITY_LEVELS_NEW3DS[] = {144, 240, 241, 360, 480};
static const int QUALITY_LEVELS_OLD3DS[] = {144, 240, 241};
static const int AUDIO_RATE_LEVELS_NEW3DS[] = {22050, 32000, 44100};
static const int AUDIO_RATE_LEVELS_OLD3DS[] = {22050, 32000, 40000};
static const int BOTTOM_DIM_LEVELS[] = {15, 30, 60, 120, 300, 0};


#include "generated/lang.inc"
#include "parts/app_core.inc"
#include "parts/text_config.inc"
#include "parts/jellyfin_api.inc"
#include "parts/websocket_remote.inc"
#include "parts/h264_player.inc"
#include "parts/mjpeg_player.inc"
#include "parts/ui.inc"

int main(void)
{
    aptHook(&g_apt_hook, app_apt_hook, NULL);
    g_apt_hooked = true;
    init_rgb565_lut();

    ui_graphics_init();
    detect_hardware();
    enable_playback_performance_mode();
    Result http_ret = httpcInit(4 * 1024 * 1024);
    if (R_SUCCEEDED(http_ret)) {
        g_http_ready = true;
    } else {
        set_status(tr(TR_STATUS_HTTP_INIT_FAIL_FMT), (unsigned long)http_ret);
    }

    load_config();
    apply_hardware_defaults();
    if (g_cfg.token[0] && g_cfg.user_id[0]) {
        if (!load_libraries()) {
            g_view = VIEW_SETUP;
        }
    } else {
        g_view = VIEW_SETUP;
    }

    while (app_keep_running()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        if (g_exit_requested || (down & KEY_START)) {
            g_exit_requested = true;
            break;
        }
        handle_input(down, held);
        if (g_exit_requested) {
            break;
        }
        g_frame_counter++;
        render();
    }

    bool system_closing = app_system_closing();
    if (!system_closing) {
        save_config();
    }
    if (!system_closing) {
        remote_control_stop();
    }
    if (!system_closing) {
        curl_http_shutdown();
    }
    if (g_http_ready && !system_closing) {
        httpcExit();
        g_http_ready = false;
    }
    if (!system_closing) {
        ui_graphics_exit();
    }
    if (g_apt_hooked && !system_closing) {
        aptUnhook(&g_apt_hook);
        g_apt_hooked = false;
    }
    return 0;
}

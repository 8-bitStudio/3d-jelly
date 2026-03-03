/**
 * 3D Jelly - Nintendo 3DS Jellyfin Client
 *
 * Direct Jellyfin connection — no proxy server required.
 * Server discovery via UDP broadcast to port 7359 (official Jellyfin protocol).
 *
 * Hardware targets:
 *   Old 3DS: ARM11 @ 268MHz, 64MB RAM, SW H.264 decode
 *   New 3DS: ARM11 @ 804MHz, 256MB RAM, HW H.264 decode (MVD)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <sys/socket.h>

#include "api.h"
#include "config.h"
#include "ui.h"
#include "player.h"

static u32 SOC_buffer[0x100000 / sizeof(u32)] __attribute__((aligned(0x1000)));

/* Global NDSP init flag — guards ndspExit() in app_exit() */
static bool g_ndsp_ok = false;

/* ── App state ───────────────────────────────────────────────────────────── */
typedef enum {
    STATE_LOADING,
    STATE_DISCOVER,      /* Server discovery (UDP broadcast) */
    STATE_AUTH,          /* Username / password */
    STATE_HOME,          /* Continue watching */
    STATE_LIBRARIES,     /* Library list */
    STATE_BROWSE,        /* Browse grid */
    STATE_ITEM_DETAIL,   /* Item detail */
    STATE_PLAYER,        /* Video playback */
    STATE_SETTINGS,      /* Settings menu */
    STATE_ERROR,         /* Error screen */
} AppState;

typedef struct {
    AppState state;
    AppState prev_state;
    ApiContext *api;
    UiContext  *ui;
    Config     *config;

    char token[256];
    char user_id[64];
    char username[64];

    ItemList *current_list;
    Item     *selected_item;
    int       scroll_offset;
    int       selected_index;

    PlayerContext *player;
    int           is_playing;

    char error_msg[256];
} AppContext;

static AppContext g_app;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void app_init(void);
static void app_exit(void);
static void app_loop(void);
static void handle_state_loading(void);
static void handle_state_discover(void);
static void handle_state_auth(void);
static void handle_state_home(void);
static void handle_state_libraries(void);
static void handle_state_browse(void);
static void handle_state_item_detail(void);
static void handle_state_player(void);
static void handle_state_settings(void);
static void set_error(const char *msg);
static void transition_to(AppState new_state);

/* ── Entry Point ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    app_init();
    app_loop();
    app_exit();
    return 0;
}

/* ── Initialization ──────────────────────────────────────────────────────── */
static void app_init(void) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.state = STATE_LOADING;

    /* Graphics */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    /* NOTE: consoleInit removed — conflicts with citro2d on same screen */

    /* Audio — non-fatal (emulators without DSP dump will fail here) */
    g_ndsp_ok = R_SUCCEEDED(ndspInit());
    if (g_ndsp_ok) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetOutputCount(1);
        ndspSetMasterVol(1.0f);
    }

    romfsInit();
    socInit(SOC_buffer, sizeof(SOC_buffer));

    /* Config */
    g_app.config = config_load(CONFIG_PATH);
    if (!g_app.config) {
        g_app.config = config_create_default();
        config_save(g_app.config, CONFIG_PATH);
    }

    /* Subsystems */
    g_app.api    = api_init(g_app.config->jellyfin_host, g_app.config->jellyfin_port);
    g_app.ui     = ui_init(g_app.config);
    g_app.player = player_init();

    /* Give player the UI's top-screen render target (not a new duplicate one) */
    player_set_render_target(g_app.player, g_app.ui->top);

    /* Initial state */
    if (!g_app.config->setup_complete || !g_app.config->jellyfin_host[0]) {
        /* First run — go to discovery screen */
        g_app.state = STATE_DISCOVER;
    } else if (!g_app.config->token[0]) {
        g_app.state = STATE_AUTH;
    } else {
        strncpy(g_app.token,    g_app.config->token,    sizeof(g_app.token) - 1);
        strncpy(g_app.user_id,  g_app.config->user_id,  sizeof(g_app.user_id) - 1);
        strncpy(g_app.username, g_app.config->username, sizeof(g_app.username) - 1);
        g_app.state = STATE_HOME;
    }
}

/* ── Main Loop ───────────────────────────────────────────────────────────── */
static void app_loop(void) {
    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();

        /* START + SELECT = force quit */
        if ((keys_held & KEY_START) && (keys_held & KEY_SELECT)) break;

        /* C3D_FrameBegin MUST wrap ALL draw calls */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        switch (g_app.state) {
            case STATE_LOADING:      handle_state_loading();      break;
            case STATE_DISCOVER:     handle_state_discover();     break;
            case STATE_AUTH:         handle_state_auth();         break;
            case STATE_HOME:         handle_state_home();         break;
            case STATE_LIBRARIES:    handle_state_libraries();    break;
            case STATE_BROWSE:       handle_state_browse();       break;
            case STATE_ITEM_DETAIL:  handle_state_item_detail();  break;
            case STATE_PLAYER:       handle_state_player();       break;
            case STATE_SETTINGS:     handle_state_settings();     break;
            case STATE_ERROR:
                ui_draw_error(g_app.ui, g_app.error_msg);
                if (keys_down & KEY_B) transition_to(g_app.prev_state);
                break;
        }

        ui_render_frame(g_app.ui);
        C3D_FrameEnd(0);
    }
}

/* ── State Handlers ──────────────────────────────────────────────────────── */

static void handle_state_loading(void) {
    ui_draw_splash(g_app.ui, "Loading...");
    /* app_init() already set the real state — only fall back if somehow still here */
    if (g_app.state == STATE_LOADING)
        g_app.state = STATE_DISCOVER;
}

static void handle_state_discover(void) {
    u32 keys_down = hidKeysDown();

    /* Trigger UDP scan on first entry */
    if (!g_app.ui->discovery_done) {
        ui_draw_splash(g_app.ui, "Scanning for Jellyfin servers...");
        C3D_FrameEnd(0);

        /* Do the actual UDP broadcast scan */
        api_discover_servers(&g_app.ui->discovery);
        g_app.ui->discovery_done = 1;
        g_app.ui->discovery_sel  = 0;
        return;
    }

    /* Draw discovery UI — returns 1 when user picks a server */
    int done = ui_draw_discovery(g_app.ui, g_app.config);

    if (done) {
        /* Update API context with selected server */
        api_set_server(g_app.api,
                       g_app.config->jellyfin_host,
                       g_app.config->jellyfin_port);
        g_app.config->setup_complete = 1;
        config_save(g_app.config, CONFIG_PATH);

        /* Clear any cached token — go back to login */
        memset(g_app.config->token,   0, sizeof(g_app.config->token));
        memset(g_app.config->user_id, 0, sizeof(g_app.config->user_id));
        config_save(g_app.config, CONFIG_PATH);

        transition_to(STATE_AUTH);
    }

    /* SELECT on discovery screen goes back to manual entry */
    if (keys_down & KEY_SELECT) {
        g_app.ui->discovery_done = 0;
        memset(&g_app.ui->discovery, 0, sizeof(g_app.ui->discovery));
    }
}

static void handle_state_auth(void) {
    AuthResult result = ui_draw_auth(g_app.ui);

    /* SELECT on auth = change server */
    u32 keys_down = hidKeysDown();
    if (keys_down & KEY_SELECT) {
        g_app.ui->discovery_done = 0;
        memset(&g_app.ui->discovery, 0, sizeof(g_app.ui->discovery));
        transition_to(STATE_DISCOVER);
        return;
    }

    if (result.done) {
        /* Draw loading screen BEFORE the blocking HTTP call.
         * Without this the GPU gets no frames during the request and freezes. */
        C3D_FrameEnd(0);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        ui_draw_splash(g_app.ui, "Signing in...");
        C3D_FrameEnd(0);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        ApiAuthResponse auth;
        int rc = api_authenticate(g_app.api, result.username, result.password, &auth);
        if (rc == 0) {
            strncpy(g_app.token,    auth.token,    sizeof(g_app.token) - 1);
            strncpy(g_app.user_id,  auth.user_id,  sizeof(g_app.user_id) - 1);
            strncpy(g_app.username, auth.username, sizeof(g_app.username) - 1);

            strncpy(g_app.config->token,    g_app.token,    sizeof(g_app.config->token) - 1);
            strncpy(g_app.config->user_id,  g_app.user_id,  sizeof(g_app.config->user_id) - 1);
            strncpy(g_app.config->username, g_app.username, sizeof(g_app.config->username) - 1);
            config_save(g_app.config, CONFIG_PATH);

            transition_to(STATE_HOME);
        } else {
            char err[64];
            snprintf(err, sizeof(err), "Login failed (error %d)", rc);
            ui_show_toast(g_app.ui, err, 3000);
        }
    }
}

static void handle_state_home(void) {
    u32 keys_down = hidKeysDown();

    if (!g_app.ui->home_loaded) {
        ItemList *resume_items = NULL;
        api_get_resume_items(g_app.api, g_app.user_id, g_app.token, &resume_items);
        ui_set_home_items(g_app.ui, resume_items);
    }

    ui_draw_home(g_app.ui, g_app.username);

    if (keys_down & KEY_A) {
        Item *item = ui_get_selected_home_item(g_app.ui);
        if (item) { g_app.selected_item = item; transition_to(STATE_ITEM_DETAIL); }
    }
    if (keys_down & KEY_L)      transition_to(STATE_LIBRARIES);
    if (keys_down & KEY_START)  transition_to(STATE_SETTINGS);
}

static void handle_state_libraries(void) {
    u32 keys_down = hidKeysDown();

    if (!g_app.ui->libraries_loaded) {
        ApiLibraryList libs;
        int rc = api_get_libraries(g_app.api, g_app.user_id, g_app.token, &libs);
        if (rc == 0) {
            ui_set_libraries(g_app.ui, &libs);
        } else {
            char err[64]; snprintf(err, sizeof(err), "Libraries failed (%d)", rc);
            set_error(err);
        }
    }

    ui_draw_libraries(g_app.ui);

    if (keys_down & KEY_A) {
        Library *lib = ui_get_selected_library(g_app.ui);
        if (lib) {
            api_set_browse_parent(g_app.api, lib->id);
            g_app.scroll_offset  = 0;
            g_app.selected_index = 0;
            transition_to(STATE_BROWSE);
        }
    }
    if (keys_down & KEY_B) transition_to(STATE_HOME);
}

static void handle_state_browse(void) {
    u32 keys_down = hidKeysDown();

    if (!g_app.ui->browse_loaded) {
        ItemList items;
        int rc = api_get_items(g_app.api, g_app.user_id, g_app.token,
                               g_app.scroll_offset, 20, &items);
        if (rc == 0) {
            ui_set_browse_items(g_app.ui, &items);
        } else {
            char err[64]; snprintf(err, sizeof(err), "Browse failed (%d)", rc);
            set_error(err);
        }
    }

    ui_draw_browse(g_app.ui);

    if (keys_down & KEY_UP)    ui_navigate(g_app.ui, DIR_UP);
    if (keys_down & KEY_DOWN)  ui_navigate(g_app.ui, DIR_DOWN);
    if (keys_down & KEY_LEFT)  ui_navigate(g_app.ui, DIR_LEFT);
    if (keys_down & KEY_RIGHT) ui_navigate(g_app.ui, DIR_RIGHT);

    if (keys_down & KEY_R) {
        g_app.scroll_offset += 20;
        g_app.ui->browse_loaded = 0;
    }
    if ((keys_down & KEY_L) && g_app.scroll_offset >= 20) {
        g_app.scroll_offset -= 20;
        g_app.ui->browse_loaded = 0;
    }

    if (keys_down & KEY_A) {
        Item *item = ui_get_selected_browse_item(g_app.ui);
        if (item) {
            g_app.selected_item = item;
            if (strcmp(item->type, "Series") == 0 || strcmp(item->type, "Season") == 0) {
                api_set_browse_parent(g_app.api, item->id);
                g_app.scroll_offset     = 0;
                g_app.ui->browse_loaded = 0;
            } else {
                transition_to(STATE_ITEM_DETAIL);
            }
        }
    }
    if (keys_down & KEY_B) {
        if (api_can_go_back(g_app.api)) {
            api_go_back(g_app.api);
            g_app.ui->browse_loaded = 0;
        } else {
            transition_to(STATE_LIBRARIES);
        }
    }
}

static void handle_state_item_detail(void) {
    u32 keys_down = hidKeysDown();

    if (!g_app.ui->detail_loaded) {
        ApiItemDetail detail;
        int rc = api_get_item_detail(g_app.api, g_app.user_id,
                                     g_app.selected_item->id, g_app.token, &detail);
        if (rc == 0) {
            ui_set_item_detail(g_app.ui, &detail);
        } else {
            char err[64]; snprintf(err, sizeof(err), "Item detail failed (%d)", rc);
            set_error(err);
        }
    }

    ui_draw_item_detail(g_app.ui);

    if (keys_down & KEY_A) transition_to(STATE_PLAYER);

    if (keys_down & KEY_X) {
        if (g_app.config->resolution_mode == RES_240P) {
            g_app.config->resolution_mode = RES_144P;
            ui_show_toast(g_app.ui, "Resolution: 144p", 1500);
        } else {
            g_app.config->resolution_mode = RES_240P;
            ui_show_toast(g_app.ui, "Resolution: 240p", 1500);
        }
        config_save(g_app.config, CONFIG_PATH);
    }
    if (keys_down & KEY_B) {
        g_app.ui->detail_loaded = 0;
        transition_to(STATE_BROWSE);
    }
}

static void handle_state_player(void) {
    u32 keys_down = hidKeysDown();

    if (!g_app.is_playing) {
        /* Choose resolution */
        int width, height;
        if (g_app.config->resolution_mode == RES_144P) {
            width = 256; height = 144;
        } else {
            width = 400; height = 240;
        }

        /* Build direct HLS URL to Jellyfin */
        char stream_url[1024];
        api_build_stream_url(g_app.api, g_app.selected_item->id, g_app.token,
                             width, height, stream_url, sizeof(stream_url));

        /* Report playback start */
        api_report_playback_start(g_app.api, g_app.selected_item->id,
                                  g_app.user_id, g_app.token);

        u64 start_ticks = g_app.selected_item->resume_ticks;

        int rc = player_start(g_app.player, stream_url, start_ticks);
        if (rc != 0) {
            set_error("Failed to start playback.");
            return;
        }
        g_app.is_playing = 1;
    }

    player_update(g_app.player);
    ui_draw_player_overlay(g_app.ui, g_app.player);

    if (keys_down & KEY_A)     player_toggle_pause(g_app.player);
    if (keys_down & KEY_RIGHT) player_seek_forward(g_app.player,  30 * 10000000LL);
    if (keys_down & KEY_LEFT)  player_seek_backward(g_app.player, 10 * 10000000LL);
    if (keys_down & KEY_UP)    player_seek_forward(g_app.player,  300 * 10000000LL);
    if (keys_down & KEY_DOWN)  player_seek_backward(g_app.player, 300 * 10000000LL);

    if (player_should_report_progress(g_app.player)) {
        api_report_playback_progress(g_app.api, g_app.selected_item->id,
                                     player_get_position_ticks(g_app.player),
                                     player_is_paused(g_app.player),
                                     g_app.user_id, g_app.token);
    }

    if (keys_down & KEY_B) {
        u64 pos = player_get_position_ticks(g_app.player);
        player_stop(g_app.player);
        api_report_playback_stop(g_app.api, g_app.selected_item->id,
                                 pos, g_app.user_id, g_app.token);
        g_app.is_playing = 0;
        g_app.ui->detail_loaded = 0;
        transition_to(STATE_ITEM_DETAIL);
    }

    if (player_has_finished(g_app.player)) {
        u64 pos = player_get_position_ticks(g_app.player);
        player_stop(g_app.player);
        api_report_playback_stop(g_app.api, g_app.selected_item->id,
                                 pos, g_app.user_id, g_app.token);
        g_app.is_playing = 0;
        transition_to(STATE_ITEM_DETAIL);
    }
}

static void handle_state_settings(void) {
    u32 keys_down = hidKeysDown();

    ui_draw_settings(g_app.ui, g_app.config);

    if (keys_down & KEY_A) {
        settings_apply_selection(g_app.ui, g_app.config);
        config_save(g_app.config, CONFIG_PATH);
        /* If they logged out, go back to discovery */
        if (!g_app.config->setup_complete || !g_app.config->token[0]) {
            g_app.ui->discovery_done = 0;
            memset(&g_app.ui->discovery, 0, sizeof(g_app.ui->discovery));
            transition_to(STATE_DISCOVER);
            return;
        }
        api_set_server(g_app.api,
                       g_app.config->jellyfin_host,
                       g_app.config->jellyfin_port);
    }
    if (keys_down & KEY_B) transition_to(STATE_HOME);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void set_error(const char *msg) {
    strncpy(g_app.error_msg, msg, sizeof(g_app.error_msg) - 1);
    transition_to(STATE_ERROR);
}

static void transition_to(AppState new_state) {
    g_app.prev_state = g_app.state;
    g_app.state      = new_state;

    if (new_state == STATE_HOME)        g_app.ui->home_loaded      = 0;
    if (new_state == STATE_LIBRARIES)   g_app.ui->libraries_loaded = 0;
    if (new_state == STATE_BROWSE)      g_app.ui->browse_loaded    = 0;
    if (new_state == STATE_ITEM_DETAIL) g_app.ui->detail_loaded    = 0;
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */
static void app_exit(void) {
    if (g_app.is_playing) player_stop(g_app.player);

    player_free(g_app.player);
    ui_free(g_app.ui);
    api_free(g_app.api);
    config_free(g_app.config);

    socExit();
    romfsExit();
    if (g_ndsp_ok) ndspExit();  /* only if init succeeded */
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}